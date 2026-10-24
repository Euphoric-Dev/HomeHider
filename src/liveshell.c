#include "liveshell.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "json.h"
#include "liveshell_elf.h"
#include "log.h"
#include "sha256.h"

static live_cfg_t C = {9021, 9050};

void live_init(const live_cfg_t *cfg) { C = *cfg; }

size_t live_elf_size(void) { return LIVESHELL_ELF_LEN; }

void live_elf_sha256(char hex[65]) { sha256_hex(LIVESHELL_ELF, LIVESHELL_ELF_LEN, hex); }

static int connect_local(int port, int timeout_s) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  struct timeval tv = {timeout_s, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof(a))) {
    close(s);
    return -1;
  }
  return s;
}

int live_get(const char *path, char **body, size_t *len) {
  *body = NULL;
  *len = 0;
  int s = connect_local(C.api_port, 15);
  if (s < 0) return -1;
  char req[1200];
  int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
  if (rl <= 0 || (size_t)rl >= sizeof(req) || send(s, req, (size_t)rl, 0) != rl) {
    close(s);
    return -1;
  }
  size_t cap = 64 * 1024, n = 0;
  char *b = malloc(cap + 1);
  if (!b) {
    close(s);
    return -1;
  }
  for (;;) {
    if (n == cap) {
      if (cap >= 16u * 1024 * 1024) break;
      char *nb = realloc(b, cap * 2 + 1);
      if (!nb) break;
      b = nb;
      cap *= 2;
    }
    ssize_t r = recv(s, b + n, cap - n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += (size_t)r;
  }
  close(s);
  b[n] = '\0';
  char *sep = strstr(b, "\r\n\r\n");
  int status = (n > 12 && !strncmp(b, "HTTP/1.", 7)) ? atoi(b + 9) : -1;
  if (!sep || status < 100) {
    free(b);
    return -1;
  }
  size_t off = (size_t)(sep + 4 - b);
  memmove(b, b + off, n - off + 1);
  *body = b;
  *len = n - off;
  return status;
}

int live_running(void) {
  int s = connect_local(C.api_port, 2);
  if (s < 0) return 0;
  close(s);
  return 1;
}

int live_start(void) {
  int s = connect_local(C.loader_port, 10);
  if (s < 0) {
    hh_log("live wallpaper: ELF loader not reachable on port %d", C.loader_port);
    return -1;
  }
  const unsigned char *p = (const unsigned char *)LIVESHELL_ELF;
  size_t off = 0, len = LIVESHELL_ELF_LEN;
  while (off < len) {
    ssize_t n = send(s, p + off, len - off, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(s);
  hh_log("live wallpaper: LiveShell %s handed to the ELF loader (%zu of %zu bytes)", LIVESHELL_VERSION,
         off, len);
  return off == len ? 0 : -1;
}

int live_stop(void) {
  char *b;
  size_t n;
  int st = live_get("/api/shutdown", &b, &n);
  free(b);
  hh_log("live wallpaper: stop requested (%d)", st);
  return st == 200 ? 0 : -1;
}

int live_video_index(const char *path) {
  char *b;
  size_t n;
  if (live_get("/api/state", &b, &n) != 200) {
    free(b);
    return -1;
  }
  int max = 40000, found = -1;
  json_tok_t *t = malloc(sizeof(json_tok_t) * (size_t)max);
  int nt = t ? json_parse(b, n, t, max) : -1;
  int vids = nt > 0 ? json_get(b, t, nt, 0, "videos") : -1;
  for (int i = 0; vids >= 0 && found < 0; i++) {
    int v = json_at(t, nt, vids, i);
    if (v < 0) break;
    char p[1024];
    if (json_str(b, t, json_get(b, t, nt, v, "path"), p, sizeof(p)) == 0 && !strcmp(p, path)) found = i;
  }
  free(t);
  free(b);
  return found;
}
