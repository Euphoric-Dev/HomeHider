#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(strbuf_t *sb) {
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
  sb->oom = 0;
}

void sb_free(strbuf_t *sb) {
  free(sb->data);
  sb_init(sb);
}

static int sb_reserve(strbuf_t *sb, size_t extra) {
  if (sb->oom) return -1;
  if (sb->len + extra + 1 <= sb->cap) return 0;
  size_t ncap = sb->cap ? sb->cap : 256;
  while (ncap < sb->len + extra + 1) ncap *= 2;
  char *n = realloc(sb->data, ncap);
  if (!n) {
    sb->oom = 1;
    return -1;
  }
  sb->data = n;
  sb->cap = ncap;
  return 0;
}

void sb_putn(strbuf_t *sb, const char *s, size_t n) {
  if (sb_reserve(sb, n)) return;
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

void sb_puts(strbuf_t *sb, const char *s) { sb_putn(sb, s, strlen(s)); }

void sb_printf(strbuf_t *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0 || sb_reserve(sb, (size_t)need)) return;
  va_start(ap, fmt);
  vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap);
  va_end(ap);
  sb->len += (size_t)need;
}

void sb_json_str(strbuf_t *sb, const char *s) {
  if (!s) {
    sb_puts(sb, "null");
    return;
  }
  sb_putn(sb, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"':  sb_puts(sb, "\\\""); break;
      case '\\': sb_puts(sb, "\\\\"); break;
      case '\n': sb_puts(sb, "\\n"); break;
      case '\r': sb_puts(sb, "\\r"); break;
      case '\t': sb_puts(sb, "\\t"); break;
      default:
        if (*p < 0x20) sb_printf(sb, "\\u%04x", *p);
        else sb_putn(sb, (const char *)p, 1);
    }
  }
  sb_putn(sb, "\"", 1);
}
