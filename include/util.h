#pragma once

#include <stddef.h>

void die(const char *fmt, ...);
int starts_with(const char *s, const char *pfx);
int validate_identifier(const char *label);
