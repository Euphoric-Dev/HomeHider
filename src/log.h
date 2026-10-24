/* Small on-disk log (/data/homehider/log.txt) shown in Diagnostics, so
 * problems on the console can be reported without a debugger. */
#pragma once

#include <stddef.h>

#include "strbuf.h"

void hh_log_init(const char *path);
void hh_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Appends the last max_bytes of the log. */
void hh_log_tail(strbuf_t *sb, size_t max_bytes);
