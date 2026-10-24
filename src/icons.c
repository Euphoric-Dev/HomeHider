#include "icons.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
/* PNG, JPEG, BMP, GIF (first frame) and TGA */
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"

#define STBIW_WINDOWS_UTF8 0
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

typedef struct {
  const char *name;
  uint8_t top[3], bottom[3];
} palette_t;

static const palette_t kPalette[] = {
    {"blue", {0x2f, 0x6d, 0xf5}, {0x10, 0x24, 0x63}},
    {"purple", {0x86, 0x4b, 0xf2}, {0x2c, 0x15, 0x63}},
    {"red", {0xe5, 0x48, 0x4d}, {0x5c, 0x12, 0x1a}},
    {"orange", {0xf0, 0x8c, 0x2e}, {0x6b, 0x2d, 0x0a}},
    {"green", {0x2f, 0xb3, 0x6b}, {0x0b, 0x45, 0x2a}},
    {"teal", {0x1f, 0xb5, 0xc4}, {0x08, 0x40, 0x4a}},
    {"gray", {0x5b, 0x63, 0x78}, {0x16, 0x19, 0x22}},
};

static const palette_t *find_color(const char *name) {
  for (size_t i = 0; i < sizeof(kPalette) / sizeof(kPalette[0]); i++)
    if (name && strcmp(name, kPalette[i].name) == 0) return &kPalette[i];
  return &kPalette[0];
}

int icon_valid_color(const char *name) {
  if (name && !strcmp(name, "none")) return 1; /* see-through: no background */
  for (size_t i = 0; i < sizeof(kPalette) / sizeof(kPalette[0]); i++)
    if (name && strcmp(name, kPalette[i].name) == 0) return 1;
  return 0;
}

/* Area-average resample of an RGBA image region (sx,sy,sw,sh) to dw x dh.
 * Colours are weighted by alpha so transparent pixels don't leave dark
 * fringes. */
static void resample(const uint8_t *src, int w, int h, int sx, int sy, int sw, int sh,
                     uint8_t *dst, int dw, int dh) {
  (void)h;
  for (int y = 0; y < dh; y++) {
    int y0 = sy + (int)((long long)y * sh / dh);
    int y1 = sy + (int)((long long)(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < dw; x++) {
      int x0 = sx + (int)((long long)x * sw / dw);
      int x1 = sx + (int)((long long)(x + 1) * sw / dw);
      if (x1 <= x0) x1 = x0 + 1;
      unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int yy = y0; yy < y1; yy++) {
        const uint8_t *row = src + ((size_t)yy * w + x0) * 4;
        for (int xx = x0; xx < x1; xx++, row += 4) {
          r += (unsigned long)row[0] * row[3];
          g += (unsigned long)row[1] * row[3];
          b += (unsigned long)row[2] * row[3];
          a += row[3];
          n++;
        }
      }
      uint8_t *o = dst + ((size_t)y * dw + x) * 4;
      if (a) {
        o[0] = (uint8_t)(r / a);
        o[1] = (uint8_t)(g / a);
        o[2] = (uint8_t)(b / a);
      } else {
        o[0] = o[1] = o[2] = 0;
      }
      o[3] = (uint8_t)(a / n);
    }
  }
}

/* EXIF orientation of a JPEG (1..8), 1 if absent. Phone photos are often
 * stored sideways with this tag saying how to turn them. */
static int jpeg_orientation(const unsigned char *d, size_t len) {
  if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
  size_t i = 2;
  while (i + 4 <= len && d[i] == 0xFF) {
    unsigned marker = d[i + 1];
    size_t seg = (size_t)d[i + 2] << 8 | d[i + 3];
    if (marker == 0xDA || seg < 2 || i + 2 + seg > len) break;
    if (marker == 0xE1 && seg >= 16 && memcmp(d + i + 4, "Exif\0\0", 6) == 0) {
      const unsigned char *t = d + i + 10;
      size_t tl = seg - 8;
      int le = t[0] == 'I';
      #define RD16(p) (le ? (unsigned)((p)[0] | (p)[1] << 8) : (unsigned)((p)[0] << 8 | (p)[1]))
      #define RD32(p) (le ? ((unsigned long)(p)[0] | (unsigned long)(p)[1] << 8 | (unsigned long)(p)[2] << 16 | (unsigned long)(p)[3] << 24) \
                          : ((unsigned long)(p)[0] << 24 | (unsigned long)(p)[1] << 16 | (unsigned long)(p)[2] << 8 | (unsigned long)(p)[3]))
      if (tl < 8) return 1;
      unsigned long ifd = RD32(t + 4);
      if (ifd + 2 > tl) return 1;
      unsigned n = RD16(t + ifd);
      for (unsigned k = 0; k < n; k++) {
        size_t e = ifd + 2 + (size_t)k * 12;
        if (e + 12 > tl) break;
        if (RD16(t + e) == 0x0112) {
          unsigned v = RD16(t + e + 8);
          return v >= 1 && v <= 8 ? (int)v : 1;
        }
      }
      #undef RD16
      #undef RD32
      return 1;
    }
    i += 2 + seg;
  }
  return 1;
}

/* Applies an EXIF orientation to an RGBA image; returns a new buffer. */
static uint8_t *orient(const uint8_t *src, int w, int h, int o, int *ow, int *oh) {
  int swap = o >= 5;
  *ow = swap ? h : w;
  *oh = swap ? w : h;
  uint8_t *dst = malloc((size_t)w * h * 4);
  if (!dst) return NULL;
  for (int y = 0; y < *oh; y++)
    for (int x = 0; x < *ow; x++) {
      int sx, sy;
      switch (o) {
        case 2: sx = w - 1 - x; sy = y; break;           /* mirror */
        case 3: sx = w - 1 - x; sy = h - 1 - y; break;   /* 180 */
        case 4: sx = x; sy = h - 1 - y; break;           /* flip */
        case 5: sx = y; sy = x; break;                   /* transpose */
        case 6: sx = y; sy = h - 1 - x; break;           /* 90 clockwise */
        case 7: sx = w - 1 - y; sy = h - 1 - x; break;   /* transverse */
        case 8: sx = w - 1 - y; sy = x; break;           /* 90 counter-clockwise */
        default: sx = x; sy = y; break;
      }
      memcpy(dst + ((size_t)y * *ow + x) * 4, src + ((size_t)sy * w + sx) * 4, 4);
    }
  return dst;
}

/* Loads an image as RGBA, upright, center-cropped to the tw:th shape and
 * scaled to tw x th. Big photos are shrunk first so memory stays small. */
static uint8_t *load_cover(const unsigned char *mem, size_t len, const char *path, int tw, int th) {
  int w, h, ch;
  uint8_t *img = mem ? stbi_load_from_memory(mem, (int)len, &w, &h, &ch, 4)
                     : stbi_load(path, &w, &h, &ch, 4);
  if (!img) return NULL;
  int o = mem ? jpeg_orientation(mem, len) : 1;

  /* shrink so the longer side is at most 2x the target (and >= target) */
  int big = tw > th ? tw : th;
  int limit = big * 2 > 1024 ? big * 2 : 1024;
  if (w > limit || h > limit) {
    int nw = w >= h ? limit : (int)((long long)w * limit / h);
    int nh = h >= w ? limit : (int)((long long)h * limit / w);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    uint8_t *small = malloc((size_t)nw * nh * 4);
    if (!small) {
      stbi_image_free(img);
      return NULL;
    }
    resample(img, w, h, 0, 0, w, h, small, nw, nh);
    stbi_image_free(img);
    img = small;
    w = nw;
    h = nh;
  } else {
    /* move into malloc memory so every path frees the same way */
    uint8_t *copy = malloc((size_t)w * h * 4);
    if (!copy) {
      stbi_image_free(img);
      return NULL;
    }
    memcpy(copy, img, (size_t)w * h * 4);
    stbi_image_free(img);
    img = copy;
  }
  if (o != 1) {
    int ow, oh;
    uint8_t *turned = orient(img, w, h, o, &ow, &oh);
    if (turned) {
      free(img);
      img = turned;
      w = ow;
      h = oh;
    }
  }
  /* largest tw:th region in the middle */
  int cw = w, chh = (int)((long long)w * th / tw);
  if (chh > h) {
    chh = h;
    cw = (int)((long long)h * tw / th);
  }
  if (cw < 1) cw = 1;
  if (chh < 1) chh = 1;
  uint8_t *out = malloc((size_t)tw * th * 4);
  if (out) resample(img, w, h, (w - cw) / 2, (h - chh) / 2, cw, chh, out, tw, th);
  free(img);
  return out;
}

static uint8_t *load_square(const unsigned char *mem, size_t len, const char *path, int size) {
  return load_cover(mem, len, path, size, size);
}

uint8_t *icon_load_cover(const unsigned char *data, size_t len, int w, int h) {
  if (!data || len < 16 || len > 32u * 1024 * 1024 || w < 1 || h < 1 || w > 8192 || h > 8192) return NULL;
  return load_cover(data, len, NULL, w, h);
}

/* Background for transparent pictures on a tile (no folder colour). */
static const palette_t kDark = {"dark", {0x2a, 0x30, 0x40}, {0x12, 0x15, 0x1d}};

/* Puts an RGBA square on a vertical gradient; returns RGB. */
static uint8_t *flatten(const uint8_t *rgba, int size, const palette_t *pal) {
  uint8_t *rgb = malloc((size_t)size * size * 3);
  if (!rgb) return NULL;
  for (int y = 0; y < size; y++) {
    uint8_t bg[3];
    for (int k = 0; k < 3; k++)
      bg[k] = (uint8_t)(pal->top[k] + (pal->bottom[k] - pal->top[k]) * y / (size > 1 ? size - 1 : 1));
    for (int x = 0; x < size; x++) {
      const uint8_t *s = rgba + ((size_t)y * size + x) * 4;
      uint8_t *d = rgb + ((size_t)y * size + x) * 3;
      for (int k = 0; k < 3; k++) d[k] = (uint8_t)((s[k] * s[3] + bg[k] * (255 - s[3])) / 255);
    }
  }
  return rgb;
}

static const palette_t *bg_of(const char *name) {
  return (!name || !strcmp(name, "dark")) ? &kDark : find_color(name);
}

static int is_none(const char *name) { return name && !strcmp(name, "none"); }

static int has_alpha(const uint8_t *rgba, int size) {
  for (size_t i = 0; i < (size_t)size * size; i++)
    if (rgba[i * 4 + 3] != 255) return 1;
  return 0;
}

/* 0..255 coverage of pixel (x,y) inside a rounded square of side s, radius r. */
static int rounded_cov(int x, int y, int s, int r) {
  int cx = x < r ? r : (x >= s - r ? s - r - 1 : x);
  int cy = y < r ? r : (y >= s - r ? s - r - 1 : y);
  if (cx == x && cy == y) return 255;
  /* 4x4 supersampling at the corners */
  int hit = 0;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      float px = x + (i + 0.5f) / 4.0f, py = y + (j + 0.5f) / 4.0f;
      float dx = px - (cx + 0.5f), dy = py - (cy + 0.5f);
      if (dx * dx + dy * dy <= (float)r * r) hit++;
    }
  return hit * 255 / 16;
}

static void blend(uint8_t *px, const uint8_t *c, int a) {
  for (int k = 0; k < 3; k++) px[k] = (uint8_t)((px[k] * (255 - a) + c[k] * a) / 255);
}

static void write_cb(void *ctx, void *data, int size) {
  struct { unsigned char *buf; size_t len, cap; int fail; } *o = ctx;
  if (o->fail) return;
  if (o->len + (size_t)size > o->cap) {
    size_t ncap = (o->cap ? o->cap * 2 : 65536);
    while (ncap < o->len + (size_t)size) ncap *= 2;
    unsigned char *nb = realloc(o->buf, ncap);
    if (!nb) { o->fail = 1; return; }
    o->buf = nb;
    o->cap = ncap;
  }
  memcpy(o->buf + o->len, data, (size_t)size);
  o->len += (size_t)size;
}

static int encode_png(const uint8_t *px, int w, int h, int comp, unsigned char **png, size_t *len) {
  struct { unsigned char *buf; size_t len, cap; int fail; } o = {NULL, 0, 0, 0};
  if (!stbi_write_png_to_func(write_cb, &o, w, h, comp, px, w * comp) || o.fail) {
    free(o.buf);
    return -1;
  }
  *png = o.buf;
  *len = o.len;
  return 0;
}

int img_encode_png(const uint8_t *px, int w, int h, int comp, unsigned char **png, size_t *len) {
  return encode_png(px, w, h, comp, png, len);
}

int img_encode_jpg(const uint8_t *rgba, int w, int h, int quality, unsigned char **jpg, size_t *len) {
  struct { unsigned char *buf; size_t len, cap; int fail; } o = {NULL, 0, 0, 0};
  if (!stbi_write_jpg_to_func(write_cb, &o, w, h, 4, rgba, quality) || o.fail) {
    free(o.buf);
    return -1;
  }
  *jpg = o.buf;
  *len = o.len;
  return 0;
}

/* Straight-alpha "over": puts colour c with coverage a (0..255) on px. */
static void over(uint8_t *px, const uint8_t *c, int a) {
  if (a <= 0) return;
  int da = px[3];
  int oa = a + da * (255 - a) / 255;
  for (int k = 0; k < 3; k++)
    px[k] = (uint8_t)(oa ? (c[k] * a + px[k] * da * (255 - a) / 255) / oa : 0);
  px[3] = (uint8_t)oa;
}

int icon_collage(const char *const *paths, int n, const char *color,
                 unsigned char **png, size_t *png_len) {
  const int S = ICON_SIZE, M = 36, G = 24, C = (S - 2 * M - G) / 2, R = 22;
  int clear = is_none(color);
  const palette_t *pal = find_color(color);
  uint8_t *canvas = malloc((size_t)S * S * 4);
  if (!canvas) return -1;

  /* background: vertical gradient, or nothing */
  for (int y = 0; y < S; y++) {
    uint8_t c[4] = {0, 0, 0, 0};
    if (!clear) {
      for (int k = 0; k < 3; k++) c[k] = (uint8_t)(pal->top[k] + (pal->bottom[k] - pal->top[k]) * y / (S - 1));
      c[3] = 255;
    }
    for (int x = 0; x < S; x++) memcpy(canvas + ((size_t)y * S + x) * 4, c, 4);
  }

  static const uint8_t white[3] = {255, 255, 255};
  static const uint8_t black[3] = {0, 0, 0};
  static const uint8_t slot[3] = {0x9a, 0xa3, 0xb5};
  for (int i = 0; i < 4; i++) {
    int ox = M + (i % 2) * (C + G), oy = M + (i / 2) * (C + G);
    uint8_t *tile = i < n && paths[i] ? load_square(NULL, 0, paths[i], C) : NULL;
    for (int y = 0; y < C; y++)
      for (int x = 0; x < C; x++) {
        int cov = rounded_cov(x, y, C, R);
        if (!cov) continue;
        uint8_t *px = canvas + ((size_t)(oy + y) * S + (ox + x)) * 4;
        if (tile) {
          const uint8_t *tp = tile + ((size_t)y * C + x) * 4;
          over(px, tp, cov * tp[3] / 255);
          /* darken the anti-aliased rim a little so light artwork keeps its shape */
          if (cov < 255 && !clear) over(px, black, (255 - cov) / 4);
        } else {
          over(px, clear ? slot : white, clear ? cov * 70 / 255 : cov * 30 / 255); /* empty slot */
        }
      }
    free(tile);
  }

  int rc;
  if (clear) {
    rc = encode_png(canvas, S, S, 4, png, png_len);
  } else {
    for (size_t p = 0; p < (size_t)S * S; p++) memmove(canvas + p * 3, canvas + p * 4, 3);
    rc = encode_png(canvas, S, S, 3, png, png_len);
  }
  free(canvas);
  return rc;
}

int icon_square(const unsigned char *data, size_t len, int size, unsigned char **png,
                size_t *png_len) {
  if (!data || len < 16 || len > 32u * 1024 * 1024 || size < 16 || size > 1024) return -1;
  uint8_t *sq = load_square(data, len, NULL, size);
  if (!sq) return -1;
  uint8_t *rgb = flatten(sq, size, &kDark);
  free(sq);
  if (!rgb) return -1;
  int rc = encode_png(rgb, size, size, 3, png, png_len);
  free(rgb);
  return rc;
}

int icon_from_image(const unsigned char *data, size_t len, const char *bg, unsigned char **png,
                    size_t *png_len) {
  if (!data || len < 16 || len > 32u * 1024 * 1024) return -1;
  uint8_t *sq = load_square(data, len, NULL, ICON_SIZE);
  if (!sq) return -1;
  int rc;
  if ((!bg || is_none(bg)) && has_alpha(sq, ICON_SIZE)) {
    rc = encode_png(sq, ICON_SIZE, ICON_SIZE, 4, png, png_len);
  } else {
    uint8_t *rgb = flatten(sq, ICON_SIZE, bg_of(bg));
    rc = rgb ? encode_png(rgb, ICON_SIZE, ICON_SIZE, 3, png, png_len) : -1;
    free(rgb);
  }
  free(sq);
  return rc;
}

int icon_flatten(const unsigned char *png, size_t len, const char *bg, unsigned char **out,
                 size_t *out_len) {
  int w, h, ch;
  uint8_t *img = stbi_load_from_memory(png, (int)len, &w, &h, &ch, 4);
  if (!img) return -1;
  int rc;
  if (w != h || is_none(bg) || !has_alpha(img, w)) {
    *out = malloc(len);
    if ((rc = *out ? 0 : -1) == 0) {
      memcpy(*out, png, len);
      *out_len = len;
    }
  } else {
    uint8_t *rgb = flatten(img, w, bg_of(bg));
    rc = rgb ? encode_png(rgb, w, h, 3, out, out_len) : -1;
    free(rgb);
  }
  stbi_image_free(img);
  return rc;
}
