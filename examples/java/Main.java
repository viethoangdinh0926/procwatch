// Minimal Java workload for the demo stack. Nothing here references
// OpenTelemetry: the whole point is that instrumentation arrives from the
// outside, via the injector setting JAVA_TOOL_OPTIONS.
//
// It serves HTTP and also calls itself on a timer, so the OpenTelemetry
// javaagent's servlet and HttpURLConnection instrumentation produce both
// server and client spans without anyone driving traffic manually.
//
// On startup the parent also forks child `java ... Main worker` processes so
// inject/wrap process-tree metrics cover more than a single JVM.

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class Main {
    private static int port() {
        String p = System.getenv("PORT");
        if (p == null || p.isEmpty()) return 8080;
        return Integer.parseInt(p);
    }

    private static int workerCount() {
        String n = System.getenv("PROCWATCH_DEMO_WORKERS");
        if (n == null || n.isEmpty()) return 2;
        try {
            return Math.max(0, Integer.parseInt(n));
        } catch (NumberFormatException e) {
            return 2;
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && "worker".equals(args[0])) {
            runWorker(args.length > 1 ? args[1] : "0");
            return;
        }

        final int PORT = port();
        HttpServer server = HttpServer.create(new InetSocketAddress(PORT), 0);
        server.createContext("/checkout", Main::handleCheckout);
        server.createContext("/health", exchange -> respond(exchange, 200, "ok"));
        server.setExecutor(Executors.newFixedThreadPool(4));
        server.start();
        System.out.println("checkout-api listening on " + PORT);

        List<Process> children = startWorkers(workerCount());
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            for (Process p : children) {
                p.destroy();
            }
        }));

        ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor();
        timer.scheduleAtFixedRate(Main::generateTraffic, 3, 5, TimeUnit.SECONDS);
    }

    private static List<Process> startWorkers(int count) throws IOException {
        List<Process> children = new ArrayList<>();
        String javaHome = System.getProperty("java.home");
        String javaBin = javaHome + "/bin/java";
        String cp = System.getProperty("java.class.path");
        for (int i = 0; i < count; i++) {
            ProcessBuilder pb = new ProcessBuilder(
                    javaBin, "-cp", cp, "Main", "worker", Integer.toString(i));
            pb.inheritIO();
            Process child = pb.start();
            children.add(child);
            System.out.println("spawned java worker-" + i + " pid=" + child.pid());
        }
        return children;
    }

    // Child JVM: burn a little CPU and retain a small heap so process metrics
    // for nested java processes show up under the same PROCWATCH_LABEL.
    private static void runWorker(String id) throws InterruptedException {
        System.out.println("java worker-" + id + " pid=" + ProcessHandle.current().pid());
        List<byte[]> retained = new ArrayList<>();
        long scratch = 0;
        while (true) {
            long end = System.nanoTime() + 50_000_000L; // ~50ms busy
            while (System.nanoTime() < end) {
                scratch += System.nanoTime();
            }
            if (retained.size() < 8) {
                retained.add(new byte[256 * 1024]);
            }
            Thread.sleep(500);
            if (scratch == 0) {
                System.out.println("java worker-" + id + " alive");
            }
        }
    }

    private static void handleCheckout(HttpExchange exchange) throws IOException {
        try {
            // Some work, so the span has a duration worth looking at.
            Thread.sleep(20 + (long) (Math.random() * 60));
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        // One in five fails, so the demo has ERROR spans to query.
        if (Math.random() < 0.2) {
            respond(exchange, 500, "upstream timeout");
        } else {
            respond(exchange, 200, "{\"order\":\"accepted\"}");
        }
    }

    private static void respond(HttpExchange exchange, int status, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        try (OutputStream out = exchange.getResponseBody()) {
            out.write(bytes);
        }
    }

    private static void generateTraffic() {
        try {
            URI uri = URI.create("http://localhost:" + port() + "/checkout");
            HttpURLConnection conn = (HttpURLConnection) uri.toURL().openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(2000);
            conn.setReadTimeout(2000);
            int code = conn.getResponseCode();
            conn.getInputStream().close();
            System.out.println("self-call -> " + code);
        } catch (Exception e) {
            System.out.println("self-call failed: " + e.getMessage());
        }
    }
}
