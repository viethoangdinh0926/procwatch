// Local NDJSON spill file + reconnect for offline DB buffering.

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/db.h"
#include "../include/db_buffer.h"

static const char *default_spill_dir(void) {
    const char *d = getenv("PROCWATCH_SPILL_DIR");
    return (d && *d) ? d : "/var/tmp/procwatch";
}

static void spill_companion(char *out, size_t cap, const char *spill, const char *suffix) {
    snprintf(out, cap, "%s%s", spill, suffix);
}

int pw_json_escape(char *out, size_t out_cap, const char *in) {
    if (!out || out_cap == 0) return -1;
    size_t o = 0;
    if (!in) in = "";
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        const char *rep = NULL;
        char tmp[7];
        switch (*p) {
            case '\\': rep = "\\\\"; break;
            case '"':  rep = "\\\""; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                    rep = tmp;
                }
                break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n + 1 > out_cap) { out[0] = '\0'; return -1; }
            memcpy(out + o, rep, n);
            o += n;
        } else {
            if (o + 2 > out_cap) { out[0] = '\0'; return -1; }
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return 0;
}

int pw_db_buf_init(pw_db_buf_t *b, const char *conninfo,
                   const char *spill_dir, const char *spill_name) {
    memset(b, 0, sizeof *b);
    snprintf(b->conninfo, sizeof b->conninfo, "%s", conninfo ? conninfo : "");
    b->reconnect_interval_sec = 5;
    const char *dir = spill_dir && *spill_dir ? spill_dir : default_spill_dir();
    if (spill_name && strchr(spill_name, '/')) {
        snprintf(b->spill_path, sizeof b->spill_path, "%s", spill_name);
    } else {
        mkdir(dir, 0755);
        snprintf(b->spill_path, sizeof b->spill_path, "%s/%s",
                 dir, spill_name && *spill_name ? spill_name : "spill.ndjson");
    }

    b->conn = db_connect(b->conninfo);
    b->online = (b->conn && PQstatus(b->conn) == CONNECTION_OK) ? 1 : 0;
    if (!b->online) {
        fprintf(stderr, "procwatch: database unreachable at start; spilling to %s\n",
                b->spill_path);
        if (b->conn) { PQfinish(b->conn); b->conn = NULL; }
    }
    return 0;
}

void pw_db_buf_close(pw_db_buf_t *b) {
    if (!b) return;
    if (b->conn) { PQfinish(b->conn); b->conn = NULL; }
    b->online = 0;
}

int pw_db_buf_ensure(pw_db_buf_t *b) {
    if (!b) return 0;
    if (b->conn && PQstatus(b->conn) == CONNECTION_OK) {
        b->online = 1;
        return 1;
    }
    b->online = 0;
    time_t now = time(NULL);
    if (b->last_reconnect_try != 0 &&
        now - b->last_reconnect_try < b->reconnect_interval_sec) {
        return 0;
    }
    b->last_reconnect_try = now;
    if (db_reconnect(&b->conn, b->conninfo)) {
        b->online = 1;
        fprintf(stderr, "procwatch: database reconnected\n");
        return 1;
    }
    fprintf(stderr, "procwatch: database reconnect failed; continuing offline\n");
    return 0;
}

int pw_db_buf_spill(pw_db_buf_t *b, const char *line) {
    if (!b || !line) return -1;
    FILE *f = fopen(b->spill_path, "a");
    if (!f) {
        ++b->spill_dropped;
        fprintf(stderr, "spill open failed (%s): %s\n", b->spill_path, strerror(errno));
        return -1;
    }
    int rc = fprintf(f, "%s\n", line);
    if (fclose(f) != 0 || rc < 0) {
        ++b->spill_dropped;
        return -1;
    }
    ++b->spilled;
    return 0;
}

int pw_db_buf_flush(pw_db_buf_t *b, pw_db_buf_replay_fn fn, void *user) {
    if (!b || !fn || !b->online || !b->conn) return -1;
    if (PQstatus(b->conn) != CONNECTION_OK) { b->online = 0; return -1; }

    char flushing_path[576], rest_path[576];
    spill_companion(flushing_path, sizeof flushing_path, b->spill_path, ".flushing");
    spill_companion(rest_path, sizeof rest_path, b->spill_path, ".rest");

    // Claim the current spill atomically so new appends go to a fresh file
    // and cannot be deleted with the batch we are draining.
    if (rename(b->spill_path, flushing_path) != 0) {
        if (errno == ENOENT) {
            unlink(flushing_path); // stale claim from a crashed prior run
            unlink(rest_path);
            return 0;
        }
        fprintf(stderr, "spill claim failed (%s): %s\n", b->spill_path, strerror(errno));
        return -1;
    }

    FILE *f = fopen(flushing_path, "r");
    if (!f) {
        // Put the file back so data is not lost.
        rename(flushing_path, b->spill_path);
        return -1;
    }

    char *line = NULL;
    size_t cap = 0;
    size_t consumed = 0;
    int stop = 0;
    FILE *rest = NULL;

    while (!stop) {
        ssize_t n = getline(&line, &cap, f);
        if (n < 0) break;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) { ++consumed; continue; }

        int rc = fn(line, (size_t)n, user);
        if (rc != 0) {
            rest = fopen(rest_path, "w");
            if (!rest) {
                free(line);
                fclose(f);
                // Failed to preserve remainder: restore original claim.
                rename(flushing_path, b->spill_path);
                return -1;
            }
            fprintf(rest, "%s\n", line);
            char *more = NULL;
            size_t mcap = 0;
            ssize_t m;
            while ((m = getline(&more, &mcap, f)) >= 0)
                fwrite(more, 1, (size_t)m, rest);
            free(more);
            if (fclose(rest) != 0) {
                free(line);
                fclose(f);
                rename(flushing_path, b->spill_path);
                unlink(rest_path);
                return -1;
            }
            stop = 1;
            break;
        }
        ++b->flushed;
        ++consumed;
    }
    free(line);
    fclose(f);

    if (stop) {
        // Prepend unflushed lines ahead of any records spilled during flush.
        FILE *cur = fopen(b->spill_path, "r");
        FILE *out = fopen(rest_path, "a");
        if (out && cur) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, cur)) > 0)
                fwrite(buf, 1, n, out);
        }
        if (cur) fclose(cur);
        if (out) fclose(out);
        unlink(b->spill_path);
        if (rename(rest_path, b->spill_path) != 0) {
            fprintf(stderr, "spill restore failed: %s\n", strerror(errno));
            // Leave .rest and .flushing for manual recovery.
            return -1;
        }
        unlink(flushing_path);
        return 1; // partial
    }

    // Full success: delete the drained batch. Any new spill_path created
    // while we were flushing is left untouched.
    if (unlink(flushing_path) != 0 && errno != ENOENT)
        fprintf(stderr, "spill cleanup failed (%s): %s\n", flushing_path, strerror(errno));
    unlink(rest_path);
    if (consumed > 0)
        fprintf(stderr, "procwatch: flushed and removed spill batch (%zu records)\n",
                consumed);
    return 0;
}

int pw_db_buf_maintain(pw_db_buf_t *b, pw_db_buf_replay_fn fn, void *user) {
    if (!pw_db_buf_ensure(b)) return 0;
    return pw_db_buf_flush(b, fn, user);
}
