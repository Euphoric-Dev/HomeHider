#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_HEADER 8192
#define MAX_BODY_DRAIN (64 * 1024)

static int send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  while (len > 0) {
    ssize_t n = send(fd, p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* Copies a header value (leading spaces skipped, up to CR/LF). */
static void header_value(const char *v, char *out, size_t outsz) {
  while (*v == ' ' || *v == '\t') v++;
  size_t n = strcspn(v, "\r\n");
  if (n >= outsz) n = outsz - 1;
  memcpy(out, v, n);
  out[n] = '\0';
}

int http_is_cross_origin(const http_req_t *req) {
  if (!req->origin[0]) return 0;              /* not from a web page */
  const char *o = req->origin;
  if (strncmp(o, "http://", 7) == 0) o += 7;
  else return 1;
  return strcmp(o, req->host) != 0;
}

int http_read_request(int fd, http_req_t *req) {
  char buf[MAX_HEADER + 1];
  size_t len = 0;
  char *end = NULL;
  memset(req, 0, sizeof(*req));

  while (len < MAX_HEADER) {
    ssize_t n = recv(fd, buf + len, MAX_HEADER - len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    len += (size_t)n;
    buf[len] = '\0';
    if ((end = strstr(buf, "\r\n\r\n"))) break;
  }
  if (!end) return -1;

  /* request line: METHOD SP TARGET SP VERSION */
  char *sp1 = strchr(buf, ' ');
  if (!sp1 || sp1 - buf >= (long)sizeof(req->method)) return -1;
  memcpy(req->method, buf, (size_t)(sp1 - buf));
  char *target = sp1 + 1;
  char *sp2 = strchr(target, ' ');
  if (!sp2) return -1;
  *sp2 = '\0';
  char *q = strchr(target, '?');
  if (q) {
    *q = '\0';
    snprintf(req->query, sizeof(req->query), "%s", q + 1);
  }
  snprintf(req->path, sizeof(req->path), "%s", target);

  /* headers we care about */
  char *cl = NULL;
  for (char *h = sp2 + 1; h < end; h++) {
    if (h != sp2 + 1 && h[-1] != '\n') continue;
    if (strncasecmp(h, "content-length:", 15) == 0) cl = h + 15;
    else if (strncasecmp(h, "host:", 5) == 0) header_value(h + 5, req->host, sizeof(req->host));
    else if (strncasecmp(h, "origin:", 7) == 0) header_value(h + 7, req->origin, sizeof(req->origin));
  }

  req->content_length = cl ? strtol(cl, NULL, 10) : -1;
  req->body_have = len - (size_t)(end + 4 - buf);
  if (req->body_have > sizeof(req->body_start)) req->body_have = sizeof(req->body_start);
  memcpy(req->body_start, end + 4, req->body_have);
  return 0;
}

void http_drain(int fd, http_req_t *req) {
  if (req->content_length <= 0 || req->content_length > MAX_BODY_DRAIN) return;
  char sink[1024];
  long left = req->content_length - (long)req->body_have;
  while (left > 0) {
    ssize_t n = recv(fd, sink, left > (long)sizeof(sink) ? sizeof(sink) : (size_t)left, 0);
    if (n <= 0) break;
    left -= n;
  }
}

int http_read_body(int fd, http_req_t *req, unsigned char **out, size_t *len, size_t max) {
  *out = NULL;
  *len = 0;
  if (req->content_length <= 0 || (size_t)req->content_length > max) return -2;
  size_t want = (size_t)req->content_length;
  unsigned char *b = malloc(want);
  if (!b) return -1;
  size_t have = req->body_have < want ? req->body_have : want;
  memcpy(b, req->body_start, have);
  while (have < want) {
    ssize_t n = recv(fd, b + have, want - have, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      free(b);
      return -1;
    }
    have += (size_t)n;
  }
  *out = b;
  *len = want;
  return 0;
}

static const char *status_text(int s) {
  switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 403: return "Forbidden";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    default:  return "Internal Server Error";
  }
}

void http_send(int fd, int status, const char *ctype, const void *body,
               size_t len, const char *extra_headers) {
  char head[768];
  const char *extra = extra_headers ? extra_headers : "";
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "%s"
                   "Connection: close\r\n"
                   "%s"
                   "\r\n",
                   status, status_text(status), ctype, len,
                   strstr(extra, "Cache-Control:") ? "" : "Cache-Control: no-store\r\n",
                   extra);
  if (n < 0 || n >= (int)sizeof(head)) return;
  if (send_all(fd, head, (size_t)n)) return;
  if (len) send_all(fd, body, len);
}

void http_send_text(int fd, int status, const char *ctype, const char *body) {
  http_send(fd, status, ctype, body, strlen(body), NULL);
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int http_query_get(const char *query, const char *key, char *out, size_t outsz) {
  size_t klen = strlen(key);
  const char *p = query;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
    if (seglen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *v = p + klen + 1;
      const char *vend = p + seglen;
      size_t o = 0;
      while (v < vend && o + 1 < outsz) {
        if (*v == '%' && vend - v >= 3 && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
          out[o++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
          v += 3;
        } else if (*v == '+') {
          out[o++] = ' ';
          v++;
        } else {
          out[o++] = *v++;
        }
      }
      out[o] = '\0';
      return 0;
    }
    p = amp ? amp + 1 : NULL;
  }
  return -1;
}
