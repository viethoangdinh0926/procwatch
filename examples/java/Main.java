// Minimal Java workload for the demo stack. Nothing here references
// OpenTelemetry: the whole point is that instrumentation arrives from the
// outside, via the injector setting JAVA_TOOL_OPTIONS.
//
// It serves HTTP and also calls itself on a timer, so the OpenTelemetry
// javaagent's servlet and HttpURLConnection instrumentation produce both
// server and client spans without anyone driving traffic manually.

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

public class Main {
    private static int port() {
        String p = System.getenv("PORT");
        if (p == null || p.isEmpty()) return 8080;
        return Integer.parseInt(p);
    }

    public static void main(String[] args) throws IOException {
        final int PORT = port();
        HttpServer server = HttpServer.create(new InetSocketAddress(PORT), 0);
        server.createContext("/checkout", Main::handleCheckout);
        server.createContext("/health", exchange -> respond(exchange, 200, "ok"));
        server.setExecutor(Executors.newFixedThreadPool(4));
        server.start();
        System.out.println("checkout-api listening on " + PORT);

        ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor();
        timer.scheduleAtFixedRate(Main::generateTraffic, 3, 5, TimeUnit.SECONDS);
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
