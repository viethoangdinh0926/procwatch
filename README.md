# ProcWatch
This is a simple application that monitors system processes and stores the data in a PostgreSQL database in TimescaleDB format. The data is then visualized using Grafana or other tools.

## Configure Postgres Server

### Install Postgres server and extensions
```bash
sudo apt install postgresql postgresql-contrib
sudo apt install postgresql-16-timescaledb-2 postgresql-16-cron
```

### Create database and user
```bash
# login to postgres
sudo -u postgres psql
```
```sql
CREATE DATABASE procwatcherdb;
CREATE USER procwatcher WITH PASSWORD 'procwatcher';
GRANT ALL PRIVILEGES ON DATABASE procwatcherdb TO procwatcher;
```

### Update `/etc/postgresql/16/main/pg_hba.conf`
- For local connection, add a line at the top of the "local" section:
  ```
  local   procwatcherdb   procwatcher   trust
  ```
- For network connection, add:
  ```
  host    procwatcherdb   procwatcher   all    md5
  ```

### Configure Postgres extensions
- Edit `/etc/postgresql/16/main/postgresql.conf`:
  - Add `shared_preload_libraries = 'pg_cron, timescaledb'`
  - Add `cron.database_name = 'procwatcherdb'` to allow creating the extension in databases other than `postgres`
- Enable the `pg_cron` extension for the target database:
  ```bash
  sudo -u postgres psql -d procwatcherdb -c "CREATE EXTENSION IF NOT EXISTS pg_cron;"
  ```
- Allow the user to do everything with the new `cron` schema:
  ```bash
  sudo -u postgres psql -d procwatcherdb -c "GRANT ALL ON SCHEMA cron TO procwatcher;"
  ```
- Restart Postgres:
  ```bash
  sudo systemctl restart postgresql
  ```

## Run Application

- Install the Postgres client library in the target environment:
  ```bash
  sudo apt install libpq-dev
  ```
- Build the application for both x86_64 and aarch64:
  ```bash
  make all
  ```
- Run the application:
  ```bash
  <binary_path> -c <command> -i <telemetry_collection_interval> -l <app_label> -d <database_connection_string> -s <database_schema>
  ```
  `<database_schema>` and `<app_label>` are used to identify the application in the database. They are used in the query of Grafana to filter the data.

## Use Grafana to Visualize Data

1. Install Grafana:
   ```bash
   sudo apt install grafana
   ```
2. Start and enable Grafana:
   ```bash
   sudo systemctl start grafana-server
   sudo systemctl enable grafana-server
   ```
3. Access Grafana at `http://<grafana_server_ip>:3000`.
4. Create a dashboard and add panels for visualization.
5. Configure each panel's query and transformation:

   **Query — visualize CPU usage:**
   ```sql
   SELECT
     ts AS time,
     pid AS metric,
     cpu_pct AS pid
   FROM <database_schema>.<table_name>
   WHERE $__timeFilter(ts)
   ORDER BY ts, metric;
   ```

   **Query — visualize memory usage:**
   ```sql
   SELECT
     ts AS time,
     pid AS metric,
     rss_kb AS pid
   FROM <database_schema>.<table_name>
   WHERE $__timeFilter(ts)
   ORDER BY ts, metric;
   ```

   **Transformation:** `Prepare time series`
   - Format: `Multi-frame time series`

## Test Application

- Run the test application:
  ```bash
  <procwatch_binary_path> -c "python3 test.py" -i 5 -l "test_app" -d "<database_connection_string>" -s "<database_schema>"
  ```

---

# Auto-Instrumentation Agent

A second, optional set of artifacts adds Dynatrace-OneAgent-style automatic
instrumentation for containerised workloads: applications get distributed
traces without their images, code, or startup commands being modified.

This is entirely additive. `make all`, `scripts/build.sh`, and the `procwatch`
binary are unchanged; the agent has its own sources, build script, and Make
targets.

## What gets built

| Artifact | Role |
| --- | --- |
| `libprocwatch_inject.so` | `LD_PRELOAD` library that detects the runtime and sets environment variables. Does no telemetry work itself. |
| `procwatch-agentd` | Daemon that receives OTLP/HTTP, decodes it, and writes spans and process metrics to TimescaleDB. |
| `java/javaagent.jar` | Upstream OpenTelemetry Java agent, loaded via `-javaagent`. |
| `python/` | `sitecustomize.py` shim plus per-ABI vendored OpenTelemetry packages. |

```
Application pod                          Node DaemonSet
┌───────────────────────────┐            ┌────────────────────────┐
│ libprocwatch_inject.so    │            │ OTLP/HTTP receiver     │
│   ├─ Java   → JAVA_TOOL_OPTIONS        │   :4318                │──┐
│   ├─ Python → PYTHONPATH  │──OTLP──────▶│                        │  │
│   └─ Go     → not injected│            │ /proc collector        │  ├─▶ TimescaleDB
└───────────────────────────┘            │   (hostPID)            │──┘
             └────────────── scraped ────▶└────────────────────────┘
```

The split is deliberate. The preloaded library is loaded into *every* process
on the node, so it only decides and sets environment variables; the actual
instrumentation is loaded afterwards by each runtime's own supported hook. A
bug in an agent then degrades one application instead of every process on the
host.

## How injection works

The injector's constructor runs before `main()` and:

1. Checks the kill switch `PROCWATCH_INJECT_DISABLED=1`.
2. Rejects anything whose executable basename is not a known runtime, using
   `getauxval(AT_EXECFN)`. Every `sh`, `cp`, and `ls` in the container stops
   here, at roughly the cost of the `mmap`.
3. Confirms the runtime with `dlsym(RTLD_DEFAULT, ...)`, probing `JLI_Launch`
   for Java and `Py_BytesMain` for CPython.
4. Rewrites the environment idempotently.

Timing is safe because ELF constructors run long before HotSpot reads
`JAVA_TOOL_OPTIONS` inside `JNI_CreateJavaVM`, and before CPython reads
`PYTHONPATH` in `config_read_env_vars`.

Idempotency is mandatory rather than defensive: the JDK launcher re-execs
itself, so the constructor runs more than once per `java` invocation, and an
unguarded append would add `-javaagent` twice.

## Runtime support and limits

| Runtime | Traces | Process metrics | Notes |
| --- | --- | --- | --- |
| Java 8+ | Yes | Yes | Upstream OpenTelemetry Java agent. |
| CPython 3.x | Yes | Yes | Needs a vendored tree matching the interpreter's exact minor version and libc. |
| Go | **No** | Yes | See below. |

**Go is metrics-only, and this is structural.** Go binaries are normally
statically linked and have no `PT_INTERP`, so `ld.so` never runs and
`LD_PRELOAD` is ignored outright. Even a cgo-enabled dynamic build gains
nothing, because the Go runtime issues syscalls directly rather than through
libc wrappers and has no equivalent of `JAVA_TOOL_OPTIONS`. Go workloads are
still visible in `<label>_procs`, collected through `/proc`, with no
privileges required inside the pod. For Go traces the options are the
OpenTelemetry Go SDK in the application, or eBPF-based instrumentation as a
privileged DaemonSet.

Other cases that are deliberately not handled:

- **setuid/setgid binaries.** The loader strips `LD_PRELOAD` under `AT_SECURE`,
  and HotSpot independently ignores `JAVA_TOOL_OPTIONS` when
  `os::have_special_privileges()`. The injector detects this and returns early.
- **Callers that build an explicit `envp`.** A parent that execs with a
  hand-built environment drops the injected variables. Interposing `execve`
  would address this and is not implemented.
- **Python ABI matching.** Wheels such as `grpcio` carry compiled extensions
  bound to one CPython version *and* one libc. The shim dispatches on
  `sys.version_info` at runtime and loads the matching `cp3XX` tree; if none
  exists it logs (under `PROCWATCH_INJECT_DEBUG=1`) and does nothing. Build
  the trees you need with `PYTHON_VERSIONS="3.11 3.12" make runtimes`.
- **Alpine/musl images need `lib-musl/libprocwatch_inject.so`.** The glibc
  build names `libc.so.6` as a dependency, which musl cannot satisfy. Loaders
  ignore an object they cannot open, so the pod starts normally and simply
  produces no traces — a silent failure worth knowing about.

A related failure mode is worth stating precisely, because it applies to
*both* libcs: a preloaded object that loads but cannot resolve a symbol
aborts the process with a relocation error, exit 127, for every command that
starts. That is why the injector links with `-Wl,--no-undefined`, keeps libc
as its only dependency, and is verified at build time to export no symbols
(an exported `getenv` or `malloc` would be interposed process-wide).

## Building

```bash
make runtimes    # download the Java agent and build the Python trees
make agent       # agentd + injector for x86_64, aarch64, and musl
make world       # everything, including the original procwatch binary
```

`make all` still builds only the `procwatch` binary.

The bundle lands in `build/agent/<arch>/`, self-contained in the same way as
the existing binary: `libpq` and its transitive dependencies are copied into
`lib/` and found through a `$ORIGIN/lib` rpath.

## Running locally

```bash
docker compose -f examples/docker-compose.yml up --build
```

This starts TimescaleDB, `procwatch-agentd`, and one workload per runtime.
Java and Python produce spans within a few seconds; all three produce process
metrics.

Standalone:

```bash
procwatch-agentd -P 4318 -i 10 -l otel -s procwatch -d "<conn_str>"
```

| Flag | Env | Default | Meaning |
| --- | --- | --- | --- |
| `-b` | `PROCWATCH_BIND` | `0.0.0.0` | Listen address |
| `-P` | `PROCWATCH_PORT` | `4318` | OTLP/HTTP port |
| `-i` | `PROCWATCH_INTERVAL` | `10` | Metric interval, seconds |
| `-l` | `PROCWATCH_LABEL` | `otel` | Table prefix |
| `-s` | `PROCWATCH_SCHEMA` | `procwatch` | Schema |
| `-d` | `PROCWATCH_DB` | see `-h` | Connection string |
| `-A` | `PROCWATCH_COLLECT_ALL` | off | Record every process, not just injected ones |
| `-M` | — | off | Receiver only, no metric collection |

Injected applications read these:

| Variable | Purpose |
| --- | --- |
| `PROCWATCH_INJECT_DISABLED=1` | Kill switch; skips all injection |
| `PROCWATCH_INJECT_DEBUG=1` | Log injection decisions to stderr |
| `PROCWATCH_AGENT_DIR` | Agent tree location (default `/opt/procwatch/agent`) |
| `PROCWATCH_ENDPOINT` | Collector endpoint (default `http://127.0.0.1:4318`) |
| `PROCWATCH_SERVICE` | Service name the collector joins metrics on |

## Kubernetes

```bash
kubectl apply -f deploy/k8s/daemonset-agentd.yaml
kubectl apply -f deploy/k8s/example-java-deployment.yaml
```

`agentd` runs as a DaemonSet with `hostPID: true`, which is what lets one
collector see every process on the node. Applications get an init container
that copies the agent tree into a shared `emptyDir`, then set `LD_PRELOAD`
and point `PROCWATCH_ENDPOINT` at `status.hostIP` through the Downward API.

A mutating admission webhook would make this automatic and is not included;
the init-container pattern is the same mechanism the OpenTelemetry Operator
uses and is far easier to debug.

## Schema

Both tables are hypertables with the same 48-hour retention and
`drop_inactive_tables` housekeeping as the existing metrics table. The
`<schema>.<label>` table used by the `procwatch` binary is untouched.

`<schema>.<label>_spans`:

| Column | Type | Notes |
| --- | --- | --- |
| `ts` | `TIMESTAMPTZ` | Span end time, not ingest time |
| `trace_id`, `span_id`, `parent_span_id` | `TEXT` | Lowercase hex; parent is `NULL` for roots |
| `name`, `kind`, `service_name`, `scope_name` | `TEXT` | `kind` is `SERVER`/`CLIENT`/… |
| `duration_ns` | `BIGINT` | |
| `status_code`, `status_message` | `TEXT` | `UNSET`/`OK`/`ERROR` |
| `attributes` | `JSONB` | Full span attributes |

`<schema>.<label>_procs`: `ts`, `service_name`, `container_id`, `pod`, `pid`,
`comm`, `runtime`, `cpu_pct`, `rss_kb`, `threads`.

`service_name` is the join key between the two: the injector plants
`PROCWATCH_SERVICE` in the process environment, and the collector reads it
back out of `/proc/<pid>/environ`, so spans and metrics agree on the name even
for Go services that emit no spans.

## Grafana queries for traces

**Slowest operations:**

```sql
SELECT
  service_name || ' ' || name AS operation,
  count(*)                                        AS calls,
  round(avg(duration_ns) / 1e6, 1)                AS avg_ms,
  round((percentile_disc(0.95) WITHIN GROUP (ORDER BY duration_ns)) / 1e6, 1) AS p95_ms
FROM <schema>.<label>_spans
WHERE $__timeFilter(ts)
GROUP BY 1
ORDER BY p95_ms DESC
LIMIT 20;
```

**Error rate over time:**

```sql
SELECT
  time_bucket('1 minute', ts) AS time,
  service_name                AS metric,
  100.0 * count(*) FILTER (WHERE status_code = 'ERROR') / NULLIF(count(*), 0) AS value
FROM <schema>.<label>_spans
WHERE $__timeFilter(ts)
GROUP BY 1, 2
ORDER BY 1;
```

**One full trace, ordered as a waterfall:**

```sql
SELECT
  ts, service_name, name, kind,
  duration_ns / 1e6 AS ms,
  status_code, span_id, parent_span_id
FROM <schema>.<label>_spans
WHERE trace_id = '$trace_id'
ORDER BY ts;
```

**Filter on a span attribute** (`attributes` is `JSONB`, so it indexes and
queries like any other JSONB column):

```sql
SELECT ts, service_name, name, attributes ->> 'http.route' AS route
FROM <schema>.<label>_spans
WHERE $__timeFilter(ts)
  AND attributes ->> 'http.status_code' = '500'
ORDER BY ts DESC;
```

**Traces and process metrics side by side**, joined on the service name. The
join is `FULL` so that Go services, which have metrics but no spans, still
appear as rows with a CPU value and no latency:

```sql
WITH spans AS (
  SELECT time_bucket('1 minute', ts) AS bucket, service_name,
         round(avg(duration_ns) / 1e6, 1) AS latency_ms
  FROM <schema>.<label>_spans
  WHERE $__timeFilter(ts)
  GROUP BY 1, 2
),
procs AS (
  SELECT time_bucket('1 minute', ts) AS bucket, service_name,
         round(avg(cpu_pct)::numeric, 1) AS cpu_pct
  FROM <schema>.<label>_procs
  WHERE $__timeFilter(ts)
  GROUP BY 1, 2
)
SELECT
  COALESCE(s.bucket, p.bucket)             AS time,
  COALESCE(s.service_name, p.service_name) AS metric,
  s.latency_ms,
  p.cpu_pct
FROM spans s
FULL JOIN procs p
  ON p.bucket = s.bucket AND p.service_name = s.service_name
ORDER BY 1;
```

## Troubleshooting

Set `PROCWATCH_INJECT_DEBUG=1` on the application; the injector and the Python
shim both explain what they decided.

| Symptom | Likely cause |
| --- | --- |
| `java -version` does not print `Picked up JAVA_TOOL_OPTIONS` | `LD_PRELOAD` not set, wrong path, or a setuid binary |
| Java starts but no spans | `javaagent.jar` missing from the agent tree; the injector skips a `-javaagent` it cannot find, because pointing the JVM at a missing jar aborts startup |
| Python initialises but emits no spans | No `cp3XX` tree for that interpreter, or a tree with no instrumentation packages |
| No spans from an Alpine pod | Using `lib/` instead of `lib-musl/` |
| No metrics for any pod | `hostPID: true` missing, or `agentd` lacks the privileges to read `/proc/<pid>/environ` |
| Metrics but only for some processes | Expected: only injected processes are recorded unless `-A` is passed |
</content>
</invoke>
