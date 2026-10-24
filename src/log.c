#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_MAX (256 * 1024)

static char g_path[300];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

void hh_log_init(const char *path) {
  snprintf(g_path, sizeof(g_path), "%s", path);
}

void hh_log(const char *fmt, ...) {
  if (!g_path[0]) return;
  pthread_mutex_lock(&g_mx);
  struct stat st;
  if (stat(g_path, &st) == 0 && st.st_size > LOG_MAX) {
    char old[320];
    snprintf(old, sizeof(old), "%s.old", g_path);
    rename(g_path, old);
  }
  FILE *f = fopen(g_path, "a");
  if (f) {
    time_t now = time(NULL);
    struct tm tm;
    char ts[32] = "";
    if (localtime_r(&now, &tm)) strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }
  pthread_mutex_unlock(&g_mx);
}

void hh_log_tail(strbuf_t *sb, size_t max_bytes) {
  pthread_mutex_lock(&g_mx);
  FILE *f = g_path[0] ? fopen(g_path, "rb") : NULL;
  if (!f) {
    pthread_mutex_unlock(&g_mx);
    sb_puts(sb, "(empty)\n");
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long from = size > (long)max_bytes ? size - (long)max_bytes : 0;
  fseek(f, from, SEEK_SET);
  char *buf = malloc((size_t)(size - from) + 1);
  if (buf) {
    size_t n = fread(buf, 1, (size_t)(size - from), f);
    buf[n] = '\0';
    char *start = buf;
    if (from > 0) { /* start at a full line */
      char *nl = strchr(buf, '\n');
      if (nl) start = nl + 1;
    }
    sb_puts(sb, start);
    free(buf);
  }
  fclose(f);
  pthread_mutex_unlock(&g_mx);
}
