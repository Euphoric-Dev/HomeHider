/* Tiny growable string buffer used for JSON / text responses. */
#pragma once

#include <stdarg.h>
#include <stddef.h>

typedef struct {
  char  *data;
  size_t len;
  size_t cap;
  int    oom;   /* set if an allocation ever failed */
} strbuf_t;

void sb_init(strbuf_t *sb);
void sb_free(strbuf_t *sb);
void sb_putn(strbuf_t *sb, const char *s, size_t n);
void sb_puts(strbuf_t *sb, const char *s);
void sb_printf(strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Appends s as a quoted, escaped JSON string ("..."). NULL becomes null. */
void sb_json_str(strbuf_t *sb, const char *s);
