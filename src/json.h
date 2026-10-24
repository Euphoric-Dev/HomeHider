/* Minimal JSON tokenizer (read-only), enough for GitHub API responses.
 * Tokens are stored in document order; a container's children follow it. */
#pragma once

#include <stddef.h>

typedef enum { JSON_OBJ = 1, JSON_ARR, JSON_STR, JSON_PRIM } json_type_t;

typedef struct {
  json_type_t type;
  int start;   /* for strings: first char after the opening quote */
  int end;     /* one past the last char (strings: the closing quote) */
} json_tok_t;

/* Returns the token count, or -1 on malformed input / too many tokens. */
int  json_parse(const char *js, size_t len, json_tok_t *toks, int max);
/* Index of the token after the whole subtree rooted at i. */
int  json_skip(const json_tok_t *t, int n, int i);
/* Value token for key in object obj, or -1. */
int  json_get(const char *js, const json_tok_t *t, int n, int obj, const char *key);
/* i-th element of array arr, or -1. */
int  json_at(const json_tok_t *t, int n, int arr, int idx);
/* Copies a string token (unescaped, UTF-8). Returns 0 on success, 1 if the
 * text was truncated to fit, -1 if i is not a valid string token. */
int  json_str(const char *js, const json_tok_t *t, int i, char *out, size_t outsz);
