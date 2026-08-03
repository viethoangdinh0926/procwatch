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
| `libprocwatch_inject.so` | `LD_PRELOAD` library: runtime env bootstrap, OTEL label attachment, metric pthread, execve env maintenance. |
| `procwatch-wrap` | Parent sampler for static binaries (e.g. Go): fork/exec the app, sample `/proc/<child>`, POST the same JSON. |
| `procwatch-agentd` | Receives OTLP/HTTP and `POST /v1/procmetrics`; creates `<label>_spans` / `<label>_procs` on first sight. |
| `java/javaagent.jar` | Upstream OpenTelemetry Java agent, loaded via `-javaagent`. |
| `python/` | `sitecustomize.py` shim plus per-ABI vendored OpenTelemetry packages. |

```
Single demo pod
┌────────────────────────────────────────┐
│ agentd :4318                           │──▶ TimescaleDB
│   OTLP /v1/traces + /v1/procmetrics    │
│                                        │
│ checkout (Java)  LD_PRELOAD ──OTLP/JSON▶│
│ payments (Python) LD_PRELOAD ─OTLP/JSON▶│
│ inventory (Go)   procwatch-wrap ─JSON──▶│
└────────────────────────────────────────┘
         (127.0.0.1 shared netns)
```

Tables are keyed by each payload’s `PROCWATCH_LABEL` (also attached to OTLP as
`procwatch.label`). `agentd` does not take `-l`; the first metric or span for a
label creates `<label>_procs` / `<label>_spans`. The optional hostPID scrape
(`-A` / `PROCWATCH_HOST_COLLECT`) is off by default so inject/wrap pushes are
not double-counted.

## How injection works

Requires a valid `PROCWATCH_LABEL` (`^[A-Za-z0-9_]+$`). Without it the injector
skips OTEL bootstrap and the metric thread.

The ELF constructor (before `main`):

1. Checks the kill switch `PROCWATCH_INJECT_DISABLED=1`.
2. Rejects unknown executable basenames via `getauxval(AT_EXECFN)`.
3. Confirms the runtime with `dlsym` (`JLI_Launch` / `Py_BytesMain`).
4. Rewrites the environment idempotently and appends
   `procwatch.label=<label>` to `OTEL_RESOURCE_ATTRIBUTES`.

Separately, `__libc_start_main` is interposed so a detached metric pthread
starts *after* the loader lock and *before* application `main`. The thread
samples `/proc/self` every `PROCWATCH_METRIC_INTERVAL` (default 10s) and POSTs
JSON to `{PROCWATCH_ENDPOINT}/v1/procmetrics` over raw sockets (no libcurl).

`execve` / `execvpe` / `execveat` are interposed so a rebuilt `envp` still
carries `PROCWATCH_LABEL`, `LD_PRELOAD`, and related vars across process-tree
re-execs (e.g. the JDK launcher).

Idempotency is mandatory: the JDK re-execs itself, so the constructor runs
more than once per `java` invocation.

## Runtime support and limits

| Runtime | Traces | Process metrics | Notes |
| --- | --- | --- | --- |
| Java 8+ | Yes | Yes (inject thread) | Upstream OpenTelemetry Java agent. |
| CPython 3.x | Yes | Yes (inject thread) | Vendored tree must match interpreter minor + libc. |
| Static Go | **No** | Yes (`procwatch-wrap`) | No `PT_INTERP`; wrap is required. |

**Static Go cannot get an in-process metric thread via `LD_PRELOAD`.** There is
no dynamic linker, so the injector never loads. Use
`command: ["/opt/procwatch/agent/bin/procwatch-wrap", "--", "<app>", …]` with
the same `PROCWATCH_LABEL` / `PROCWATCH_ENDPOINT`. For Go traces, use the
OpenTelemetry Go SDK in-app or a privileged eBPF agent; both are out of scope
here.

Other cases deliberately not handled:

- **setuid/setgid binaries.** The loader strips `LD_PRELOAD` under `AT_SECURE`.
- **Python ABI matching.** Build the trees you need with
  `PYTHON_VERSIONS="3.11 3.12" make runtimes`.
- **Alpine/musl images need the musl injector**
  (`build/agent/<arch>-musl/lib/…`). A glibc `.so` is a silent no-op on musl.
- **Automatic Go entrypoint rewrite.** Manifests must use `procwatch-wrap`
  manually (no mutating webhook yet).

The injector links with `-Wl,--no-undefined`, keeps libc as its only
`DT_NEEDED`, and exports only the interposed symbols (`__libc_start_main`,
`execve`, `execvpe`, `execveat`). An unresolved preload symbol aborts every
process with exit 127.
## Building

```bash
make runtimes           # download the Java agent and build the Python trees
make agent-x86_64       # agentd + glibc injector + wrap
make agent-musl-x86_64  # musl injector + musl-static wrap (needed for Alpine Go)
make agent              # all arches (glibc + musl)
make world              # everything, including the original procwatch binary
```

`procwatch-wrap` must be the musl-static build to run inside Alpine/scratch Go
images; `make agent-musl-x86_64` writes that binary into
`build/agent/x86_64/bin/procwatch-wrap`.

`make all` still builds only the `procwatch` binary.

The bundle lands in `build/agent/<arch>/`, self-contained in the same way as
the existing binary: `libpq` and its transitive dependencies are copied into
`lib/` and found through a `$ORIGIN/lib` rpath.

## Running locally

```bash
make runtimes && make agent-x86_64 && make agent-musl-x86_64
docker compose -f examples/docker-compose.yml -p pwdemo up --build
```

Java/Python produce spans and inject-thread metrics into `checkout_*` /
`payments_*`. Go uses `procwatch-wrap` and lands metrics in `inventory_procs`.
`agentd` is started with no `-l`; tables appear on first payload.

Standalone:

```bash
procwatch-agentd -P 4318 -s procwatch -d "<conn_str>"
```

| Flag | Env | Default | Meaning |
| --- | --- | --- | --- |
| `-b` | `PROCWATCH_BIND` | `0.0.0.0` | Listen address |
| `-P` | `PROCWATCH_PORT` | `4318` | OTLP/HTTP + procmetrics port |
| `-i` | `PROCWATCH_INTERVAL` | `10` | Host-scrape interval when `-A` is set |
| `-s` | `PROCWATCH_SCHEMA` | `procwatch` | Schema |
| `-d` | `PROCWATCH_DB` | see `-h` | Connection string |
| `-R` | `PROCWATCH_RETENTION_HOURS` | `168` | Timescale chunk retention (hours) |
| `-T` | `PROCWATCH_INACTIVE_HOURS` | `720` | Drop tables with no writes for this many hours |
| `-A` | `PROCWATCH_HOST_COLLECT` | off | Optional node-wide `/proc` scrape of labeled procs |

Application / wrap env:

| Variable | Purpose |
| --- | --- |
| `PROCWATCH_LABEL` | **Required.** Table key; also set as OTLP `procwatch.label` |
| `PROCWATCH_INJECT_DISABLED=1` | Kill switch; skips injection + metric thread |
| `PROCWATCH_INJECT_DEBUG=1` | Log injection decisions to stderr |
| `PROCWATCH_AGENT_DIR` | Agent tree location (default `/opt/procwatch/agent`) |
| `PROCWATCH_ENDPOINT` | Collector base URL (default `http://127.0.0.1:4318`) |
| `PROCWATCH_SERVICE` | Service name on metrics (and OTEL service.name when set) |
| `PROCWATCH_METRIC_INTERVAL` | Inject/wrap sample period, seconds (default 10) |
| `PROCWATCH_SPILL_DIR` | Offline NDJSON spill directory (default `/var/tmp/procwatch`) |

## Kubernetes

```bash
kubectl apply -f deploy/k8s/example-apps-deployment.yaml
```

The example Deployment is a single pod with `agentd`, Java, Python, and Go.
Apps send OTLP and `/v1/procmetrics` to `http://127.0.0.1:4318` (shared
network namespace). Java/Python set `LD_PRELOAD`; Go uses `procwatch-wrap`.
Each app container requires its own `PROCWATCH_LABEL` and binds a distinct
`PORT`. Replace the `procwatch-db` Secret with your TimescaleDB connection
string before applying.

## Schema

Tables are created dynamically per label. Hypertables use chunk retention
controlled by `-R` / `PROCWATCH_RETENTION_HOURS` (default **168** hours).
`drop_inactive_tables` removes tables with no writes for `-T` /
`PROCWATCH_INACTIVE_HOURS` (default **720** hours). Both apply to the classic
`procwatch` binary as well (`-R` / `-T`). The `<schema>.<label>` table used by
standalone `procwatch` is untouched.

If TimescaleDB is unreachable, `procwatch` and `agentd` append records to an
NDJSON spill file under `PROCWATCH_SPILL_DIR` (default `/var/tmp/procwatch`),
retry the connection about every 5 seconds, and flush the spill in order once
the database is back.

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

`service_name` joins the two tables. Spans without `procwatch.label` are
rejected so unlabeled data cannot land in a mystery table.
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
| Injector skips everything | Missing/invalid `PROCWATCH_LABEL` |
| `java -version` does not print `Picked up JAVA_TOOL_OPTIONS` | `LD_PRELOAD` not set, wrong path, or a setuid binary |
| Java starts but no spans | `javaagent.jar` missing; injector skips a `-javaagent` it cannot find |
| Python initialises but emits no spans | No `cp3XX` tree for that interpreter |
| No spans from an Alpine pod | Using glibc injector instead of musl |
| Spans rejected / no `_spans` table | OTLP resource missing `procwatch.label` (injector did not attach it) |
| No `_procs` rows | Endpoint unreachable from the inject thread / wrap, or label never POSTed |
| Go has no metrics | Entrypoint not wrapped with `procwatch-wrap` |
</content>
</invoke>
