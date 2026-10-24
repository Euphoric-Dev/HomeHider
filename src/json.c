#include "json.h"

#include <string.h>

#define MAX_DEPTH 64

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int json_parse(const char *js, size_t len, json_tok_t *toks, int max) {
  int n = 0, depth = 0;
  int stack[MAX_DEPTH];         /* open container token indexes */
  size_t i = 0;

  while (i < len) {
    char c = js[i];
    if (is_ws(c) || c == ',' || c == ':') {
      i++;
      continue;
    }
    if (c == '{' || c == '[') {
      if (n >= max || depth >= MAX_DEPTH) return -1;
      toks[n].type = c == '{' ? JSON_OBJ : JSON_ARR;
      toks[n].start = (int)i;
      toks[n].end = -1;
      stack[depth++] = n++;
      i++;
      continue;
    }
    if (c == '}' || c == ']') {
      if (depth == 0) return -1;
      int open = stack[--depth];
      if ((c == '}') != (toks[open].type == JSON_OBJ)) return -1;
      toks[open].end = (int)i + 1;
      i++;
      continue;
    }
    if (c == '"') {
      size_t j = i + 1;
      while (j < len && js[j] != '"') {
        if (js[j] == '\\') j++;
        j++;
      }
      if (j >= len) return -1;
      if (n >= max) return -1;
      toks[n].type = JSON_STR;
      toks[n].start = (int)i + 1;
      toks[n].end = (int)j;
      n++;
      i = j + 1;
      continue;
    }
    /* number, true, false, null */
    size_t j = i;
    while (j < len && !is_ws(js[j]) && js[j] != ',' && js[j] != ']' &&
           js[j] != '}' && js[j] != ':')
      j++;
    if (j == i) return -1;
    if (n >= max) return -1;
    toks[n].type = JSON_PRIM;
    toks[n].start = (int)i;
    toks[n].end = (int)j;
    n++;
    i = j;
  }
  return depth == 0 ? n : -1;
}

/* Strings end at the closing quote; compare positions consistently. */
static int tok_end(const json_tok_t *t) {
  return t->type == JSON_STR ? t->end + 1 : t->end;
}

static int tok_begin(const json_tok_t *t) {
  return t->type == JSON_STR ? t->start - 1 : t->start;
}

int json_skip(const json_tok_t *t, int n, int i) {
  int end = tok_end(&t[i]);
  int j = i + 1;
  while (j < n && tok_begin(&t[j]) < end) j++;
  return j;
}

int json_get(const char *js, const json_tok_t *t, int n, int obj, const char *key) {
  if (obj < 0 || obj >= n || t[obj].type != JSON_OBJ) return -1;
  size_t klen = strlen(key);
  int end = tok_end(&t[obj]);
  int k = obj + 1;
  while (k < n && tok_begin(&t[k]) < end) {
    int v = k + 1;
    if (v >= n) return -1;
    if (t[k].type == JSON_STR && (size_t)(t[k].end - t[k].start) == klen &&
        memcmp(js + t[k].start, key, klen) == 0)
      return v;
    k = json_skip(t, n, v);
  }
  return -1;
}

int json_at(const json_tok_t *t, int n, int arr, int idx) {
  if (arr < 0 || arr >= n || t[arr].type != JSON_ARR) return -1;
  int end = tok_end(&t[arr]);
  int k = arr + 1, c = 0;
  while (k < n && tok_begin(&t[k]) < end) {
    if (c == idx) return k;
    c++;
    k = json_skip(t, n, k);
  }
  return -1;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

static size_t put_utf8(char *o, unsigned cp) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

int json_str(const char *js, const json_tok_t *t, int i, char *out, size_t outsz) {
  if (i < 0 || t[i].type != JSON_STR || outsz == 0) return -1;
  const char *p = js + t[i].start, *e = js + t[i].end;
  size_t o = 0;
  while (p < e) {
    char tmp[4];
    size_t w = 0;
    if (*p != '\\') {
      tmp[0] = *p++;
      w = 1;
    } else {
      if (p + 1 >= e) return -1;
      char c = p[1];
      p += 2;
      switch (c) {
        case 'n': tmp[0] = '\n'; w = 1; break;
        case 't': tmp[0] = '\t'; w = 1; break;
        case 'r': tmp[0] = '\r'; w = 1; break;
        case 'b': tmp[0] = '\b'; w = 1; break;
        case 'f': tmp[0] = '\f'; w = 1; break;
        case 'u': {
          unsigned cp;
          if (e - p < 4 || hex4(p, &cp)) return -1;
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            unsigned lo;
            if (!hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            }
          }
          if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
          w = put_utf8(tmp, cp);
          break;
        }
        default: tmp[0] = c; w = 1; break;   /* \" \\ \/ */
      }
    }
    if (o + w >= outsz) {   /* truncate, never split a UTF-8 sequence */
      out[o] = '\0';
      return 1;
    }
    memcpy(out + o, tmp, w);
    o += w;
  }
  out[o] = '\0';
  return 0;
}
