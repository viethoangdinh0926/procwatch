#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "../include/util.h"

void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

int starts_with(const char *s, const char *pfx) {
    return strncmp(s, pfx, strlen(pfx)) == 0;
}

int validate_identifier(const char *label) {
    if (!label) return 0;
    size_t n = strlen(label);
    if (n == 0 || n >= 100) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)label[i];
        if (!(isalnum(c) || c == '_')) return 0;
    }
    return 1;
}
