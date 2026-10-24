/* Minimal blocking HTTP/1.1 helpers (one request per connection). */
#pragma once

#include <stddef.h>

typedef struct {
  char method[8];
  char path[256];
  char query[2048];
  char host[128];    /* Host header, if sent */
  char origin[160];  /* Origin header, if sent */
  long content_length;        /* -1 if absent */
  size_t body_have;           /* body bytes already received with headers */
  unsigned char body_start[8192];
} http_req_t;

/* Reads and parses the request line + headers. The body is left for
 * http_read_body() or http_drain(). Returns 0 on success. */
int  http_read_request(int fd, http_req_t *req);
/* Reads the whole body (up to max bytes) into a malloc'd buffer.
 * Returns 0 on success, -2 if larger than max or missing length, -1 on error. */
int  http_read_body(int fd, http_req_t *req, unsigned char **out, size_t *len, size_t max);
/* Reads and discards a small body so the client doesn't see a reset. */
void http_drain(int fd, http_req_t *req);

/* 1 if the request came from another web site (Origin doesn't match Host). */
int  http_is_cross_origin(const http_req_t *req);

void http_send(int fd, int status, const char *ctype, const void *body,
               size_t len, const char *extra_headers);
void http_send_text(int fd, int status, const char *ctype, const char *body);

/* Copies the URL-decoded value of key from a query string. Returns 0 if found. */
int  http_query_get(const char *query, const char *key, char *out, size_t outsz);
