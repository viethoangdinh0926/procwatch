#pragma once

// Local NDJSON spill + reconnect helper shared by procwatch and agentd.
// When the database is unreachable, records are appended under spill_dir;
// pw_db_buf_maintain periodically reconnects and replays the file.

#include <postgresql/libpq-fe.h>
#include <time.h>

#ifndef PW_DB_BUF_H
#define PW_DB_BUF_H

typedef struct {
    PGconn *conn;
    char conninfo[1024];
    char spill_path[512];
    int online;
    time_t last_reconnect_try;
    int reconnect_interval_sec;
    unsigned long long spilled;
    unsigned long long flushed;
    unsigned long long spill_dropped;
} pw_db_buf_t;

// spill_name is a file basename under spill_dir (or absolute path if it
// contains '/'). Default spill_dir: PROCWATCH_SPILL_DIR or /var/tmp/procwatch.
int pw_db_buf_init(pw_db_buf_t *b, const char *conninfo,
                   const char *spill_dir, const char *spill_name);
void pw_db_buf_close(pw_db_buf_t *b);

// Attempt reconnect if offline (rate-limited). Returns 1 if CONNECTION_OK.
int pw_db_buf_ensure(pw_db_buf_t *b);

// Append one NDJSON line (without trailing newline). Returns 0 on success.
int pw_db_buf_spill(pw_db_buf_t *b, const char *line);

// Replay callback: return 0 to consume the line, -1 to stop flush (line kept).
typedef int (*pw_db_buf_replay_fn)(const char *line, size_t len, void *user);

// If online, replay spill file FIFO. Successfully consumed prefix is truncated.
int pw_db_buf_flush(pw_db_buf_t *b, pw_db_buf_replay_fn fn, void *user);

// ensure + flush. Call from the main loop.
int pw_db_buf_maintain(pw_db_buf_t *b, pw_db_buf_replay_fn fn, void *user);

// JSON string escape into out; returns 0 on success, -1 if truncated.
int pw_json_escape(char *out, size_t out_cap, const char *in);

#endif
