# ProcWatch

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

- Install the Postgres client library:
  ```bash
  sudo apt install libpq-dev
  ```
- Run the application:
  ```bash
  <binary_path> -c <command> -i <telemetry collection interval> -l <app label> -d <database connection string> -s <database schema>
  ```

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
4. Log in with the username and password from `info.txt`.
5. Add a Postgres data source using the connection string from `info.txt`.
6. Create a dashboard and add panels for visualization.
7. Configure each panel's query and transformation:

   **Query — visualize CPU usage:**
   ```sql
   SELECT
     ts AS time,
     pid AS metric,
     cpu_pct AS pid
   FROM procwatch.<table_name>
   WHERE $__timeFilter(ts)
   ORDER BY ts, metric;
   ```

   **Query — visualize memory usage:**
   ```sql
   SELECT
     ts AS time,
     pid AS metric,
     rss_kb AS pid
   FROM procwatch.<table_name>
   WHERE $__timeFilter(ts)
   ORDER BY ts, metric;
   ```

   **Transformation:** `Prepare time series`
   - Format: `Multi-frame time series`

## Test Application

- Run the test application:
  ```bash
  python3 test.py
  ```
</content>
</invoke>
