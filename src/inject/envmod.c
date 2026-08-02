// Idempotent environment editing for the injector.
//
// Idempotency is not a nicety here: the JDK launcher re-execs itself
// (unconditionally under musl, and under glibc whenever LD_LIBRARY_PATH is
// set and its prefix does not already match), passing the current environ
// through. The constructor therefore runs at least twice for a typical
// `java` invocation, and an unguarded append would add -javaagent twice.

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "../../include/inject.h"

#define PW_ENV_MAX 8192

int pw_env_append(const char *name, const char *addition, const char *marker) {
    const char *cur = getenv(name);
    if (cur && marker && strstr(cur, marker)) {
        pw_debug("%s already carries %s, leaving it alone", name, marker);
        return 0;
    }
    if (!cur || !*cur) return setenv(name, addition, 1);

    size_t curlen = strlen(cur), addlen = strlen(addition);
    char buf[PW_ENV_MAX];
    if (curlen + addlen + 1 > sizeof buf) {
        pw_debug("%s too long to extend, skipping", name);
        return -1;
    }
    memcpy(buf, cur, curlen);
    memcpy(buf + curlen, addition, addlen);
    buf[curlen + addlen] = '\0';
    return setenv(name, buf, 1);
}

int pw_env_prepend(const char *name, const char *addition, const char *marker,
                   char separator) {
    const char *cur = getenv(name);
    if (cur && marker && strstr(cur, marker)) {
        pw_debug("%s already carries %s, leaving it alone", name, marker);
        return 0;
    }
    if (!cur || !*cur) return setenv(name, addition, 1);

    size_t curlen = strlen(cur), addlen = strlen(addition);
    char buf[PW_ENV_MAX];
    if (curlen + addlen + 2 > sizeof buf) {
        pw_debug("%s too long to extend, skipping", name);
        return -1;
    }
    memcpy(buf, addition, addlen);
    buf[addlen] = separator;
    memcpy(buf + addlen + 1, cur, curlen);
    buf[addlen + 1 + curlen] = '\0';
    return setenv(name, buf, 1);
}

int pw_env_set_default(const char *name, const char *value) {
    // Deliberately non-overwriting: anything the operator set explicitly on
    // the pod spec outranks our default.
    return setenv(name, value, 0);
}
