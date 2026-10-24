#include "texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "icons.h"

/* DXGI formats Home Hider can write */
#define DXGI_RGBA8       28
#define DXGI_RGBA8_SRGB  29
#define DXGI_BC1         71
#define DXGI_BC1_SRGB    72
#define DXGI_BC3         77
#define DXGI_BC3_SRGB    78
#define DXGI_BGRA8       87
#define DXGI_BGRA8_SRGB  91
#define DXGI_BC7         98
#define DXGI_BC7_SRGB    99

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------------- */
/* probing                                                                   */
/* ------------------------------------------------------------------------- */

static const char *ext_of(const char *path, char *buf, size_t n) {
  size_t len = strcspn(path, "?#");
  const char *slash = NULL, *dot = NULL;
  for (size_t i = 0; i < len; i++) {
    if (path[i] == '/') slash = path + i;
    if (path[i] == '.') dot = path + i;
  }
  if (!dot || (slash && dot < slash)) return "";
  size_t el = (size_t)(path + len - dot - 1);
  if (el >= n) return "";
  memcpy(buf, dot + 1, el);
  buf[el] = '\0';
  return buf;
}

static int sane(int v) { return v >= 16 && v <= 8192; }

int tex_probe(const char *path, tex_fmt_t *f) {
  memset(f, 0, sizeof(*f));
  char eb[16];
  const char *ext = ext_of(path, eb, sizeof(eb));
  if (!strcasecmp(ext, "dds")) f->kind = TEX_DDS;
  else if (!strcasecmp(ext, "png")) f->kind = TEX_PNG;
  else if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) f->kind = TEX_JPG;
  else return -1;
  /* defaults: a 4K background, BC7 for DDS */
  f->w = TEX_BG_W;
  f->h = TEX_BG_H;
  f->mips = 1;
  f->dxgi = f->kind == TEX_DDS ? DXGI_BC7 : 0;

  char clean[600];
  size_t len = strcspn(path, "?#");
  if (len >= sizeof(clean)) return 0;
  memcpy(clean, path, len);
  clean[len] = '\0';
  FILE *fp = fopen(clean, "rb");
  if (!fp) return 0; /* inside a game package we can't read: keep the defaults */
  size_t cap = f->kind == TEX_JPG ? 256 * 1024 : 160;
  uint8_t *b = malloc(cap);
  size_t n = b ? fread(b, 1, cap, fp) : 0;
  fclose(fp);
  if (!b) return 0;

  if (f->kind == TEX_DDS && n >= 128 && !memcmp(b, "DDS ", 4)) {
    int h = (int)rd32(b + 12), w = (int)rd32(b + 16), mips = (int)rd32(b + 28);
    uint32_t pflags = rd32(b + 80), bits = rd32(b + 88), rmask = rd32(b + 92);
    if (sane(w) && sane(h)) {
      f->w = w;
      f->h = h;
    }
    f->mips = mips >= 1 && mips <= 16 ? mips : 1;
    if (!memcmp(b + 84, "DX10", 4) && n >= 148) {
      f->dxgi = rd32(b + 128);
    } else if (!memcmp(b + 84, "DXT1", 4)) {
      f->dxgi = DXGI_BC1;
      f->legacy = 1;
    } else if (!memcmp(b + 84, "DXT5", 4)) {
      f->dxgi = DXGI_BC3;
      f->legacy = 1;
    } else if ((pflags & 0x40) && bits == 32 && rmask == 0x000000ffu) {
      f->dxgi = DXGI_RGBA8;
      f->legacy = 1;
    } else if ((pflags & 0x40) && bits == 32 && rmask == 0x00ff0000u) {
      f->dxgi = DXGI_BGRA8;
      f->legacy = 1;
    } else {
      f->dxgi = 0; /* something else: not supported */
    }
  } else if (f->kind == TEX_PNG && n >= 24 && !memcmp(b, "\x89PNG", 4)) {
    int w = (int)(b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19]);
    int h = (int)(b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23]);
    if (sane(w) && sane(h)) {
      f->w = w;
      f->h = h;
    }
  } else if (f->kind == TEX_JPG && n > 4 && b[0] == 0xFF && b[1] == 0xD8) {
    size_t i = 2;
    while (i + 9 < n && b[i] == 0xFF) {
      uint8_t m = b[i + 1];
      size_t seg = (size_t)b[i + 2] << 8 | b[i + 3];
      if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
        int h = b[i + 5] << 8 | b[i + 6], w = b[i + 7] << 8 | b[i + 8];
        if (sane(w) && sane(h)) {
          f->w = w;
          f->h = h;
        }
        break;
      }
      i += 2 + seg;
    }
  }
  free(b);
  return 0;
}

int tex_supported(const tex_fmt_t *f) {
  if (f->kind == TEX_PNG || f->kind == TEX_JPG) return 1;
  if (f->kind != TEX_DDS) return 0;
  switch (f->dxgi) {
    case DXGI_BC7: case DXGI_BC7_SRGB:
    case DXGI_BC1: case DXGI_BC1_SRGB:
    case DXGI_BC3: case DXGI_BC3_SRGB:
    case DXGI_RGBA8: case DXGI_RGBA8_SRGB:
    case DXGI_BGRA8: case DXGI_BGRA8_SRGB:
      return 1;
    default:
      return 0;
  }
}

static const char *dxgi_name(unsigned d) {
  switch (d) {
    case DXGI_BC7: case DXGI_BC7_SRGB: return "BC7";
    case DXGI_BC1: case DXGI_BC1_SRGB: return "BC1";
    case DXGI_BC3: case DXGI_BC3_SRGB: return "BC3";
    case DXGI_RGBA8: case DXGI_RGBA8_SRGB: return "RGBA8";
    case DXGI_BGRA8: case DXGI_BGRA8_SRGB: return "BGRA8";
    default: return "format";
  }
}

void tex_describe(const tex_fmt_t *f, char *out, size_t n) {
  if (f->kind == TEX_DDS)
    snprintf(out, n, "DDS %s%s (dxgi %u) %dx%d, %d mip%s", dxgi_name(f->dxgi),
             f->legacy ? " legacy" : "", f->dxgi, f->w, f->h, f->mips, f->mips == 1 ? "" : "s");
  else
    snprintf(out, n, "%s %dx%d", f->kind == TEX_PNG ? "PNG" : "JPEG", f->w, f->h);
}

const char *tex_ext(const tex_fmt_t *f) {
  return f->kind == TEX_DDS ? "dds" : f->kind == TEX_PNG ? "png" : "jpg";
}

/* ------------------------------------------------------------------------- */
/* block compression                                                         */
/* ------------------------------------------------------------------------- */

/* 4x4 block at (bx,by), edge pixels repeated past the border. */
static void get_block(const uint8_t *img, int w, int h, int bx, int by, uint8_t px[16][4]) {
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) {
      int sx = bx + x < w ? bx + x : w - 1, sy = by + y < h ? by + y : h - 1;
      memcpy(px[y * 4 + x], img + ((size_t)sy * w + sx) * 4, 4);
    }
}

/* Endpoints along the main colour axis of the block (nc channels). */
static void axis_endpoints(const uint8_t px[16][4], int nc, float e0[4], float e1[4]) {
  float mean[4] = {0, 0, 0, 0};
  for (int i = 0; i < 16; i++)
    for (int c = 0; c < nc; c++) mean[c] += px[i][c] / 16.0f;
  float cov[4][4] = {{0}};
  for (int i = 0; i < 16; i++)
    for (int a = 0; a < nc; a++)
      for (int b = 0; b < nc; b++) cov[a][b] += (px[i][a] - mean[a]) * (px[i][b] - mean[b]);
  float ax[4] = {1, 1, 1, 1};
  for (int it = 0; it < 8; it++) {
    float nx[4] = {0, 0, 0, 0}, len = 0;
    for (int a = 0; a < nc; a++)
      for (int b = 0; b < nc; b++) nx[a] += cov[a][b] * ax[b];
    for (int a = 0; a < nc; a++) len += nx[a] * nx[a];
    if (len < 1e-6f) break;
    len = sqrtf(len);
    for (int a = 0; a < nc; a++) ax[a] = nx[a] / len;
  }
  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < 16; i++) {
    float t = 0;
    for (int c = 0; c < nc; c++) t += (px[i][c] - mean[c]) * ax[c];
    if (t < tmin) tmin = t;
    if (t > tmax) tmax = t;
  }
  for (int c = 0; c < 4; c++) {
    float a = c < nc ? ax[c] : 0;
    e0[c] = mean[c] + a * tmin;
    e1[c] = mean[c] + a * tmax;
    if (e0[c] < 0) e0[c] = 0;
    if (e0[c] > 255) e0[c] = 255;
    if (e1[c] < 0) e1[c] = 0;
    if (e1[c] > 255) e1[c] = 255;
  }
}

/* --- BC1 / BC3 --- */

static uint16_t to565(const float c[4]) {
  int r = (int)(c[0] * 31 / 255 + 0.5f), g = (int)(c[1] * 63 / 255 + 0.5f), b = (int)(c[2] * 31 / 255 + 0.5f);
  return (uint16_t)(r << 11 | g << 5 | b);
}

static void from565(uint16_t v, int out[3]) {
  int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
  out[0] = r << 3 | r >> 2;
  out[1] = g << 2 | g >> 4;
  out[2] = b << 3 | b >> 2;
}

static void bc1_block(const uint8_t px[16][4], uint8_t out[8]) {
  float e0[4], e1[4];
  axis_endpoints(px, 3, e0, e1);
  uint16_t c0 = to565(e1), c1 = to565(e0);
  if (c0 < c1) {
    uint16_t t = c0;
    c0 = c1;
    c1 = t;
  }
  int pal[4][3];
  from565(c0, pal[0]);
  from565(c1, pal[1]);
  for (int k = 0; k < 3; k++) {
    pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
    pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
  }
  uint32_t idx = 0;
  if (c0 != c1)
    for (int i = 0; i < 16; i++) {
      int best = 0, bd = 1 << 30;
      for (int p = 0; p < 4; p++) {
        int d = 0;
        for (int k = 0; k < 3; k++) d += (px[i][k] - pal[p][k]) * (px[i][k] - pal[p][k]);
        if (d < bd) {
          bd = d;
          best = p;
        }
      }
      idx |= (uint32_t)best << (2 * i);
    }
  out[0] = (uint8_t)c0;
  out[1] = (uint8_t)(c0 >> 8);
  out[2] = (uint8_t)c1;
  out[3] = (uint8_t)(c1 >> 8);
  wr32(out + 4, idx);
}

static void bc4_alpha(const uint8_t px[16][4], uint8_t out[8]) {
  int a0 = 0, a1 = 255;
  for (int i = 0; i < 16; i++) {
    if (px[i][3] > a0) a0 = px[i][3];
    if (px[i][3] < a1) a1 = px[i][3];
  }
  out[0] = (uint8_t)a0;
  out[1] = (uint8_t)a1;
  uint64_t bits = 0;
  if (a0 != a1) {
    int pal[8] = {a0, a1};
    for (int k = 1; k <= 6; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    for (int i = 0; i < 16; i++) {
      int best = 0, bd = 1 << 30;
      for (int p = 0; p < 8; p++) {
        int d = abs(px[i][3] - pal[p]);
        if (d < bd) {
          bd = d;
          best = p;
        }
      }
      bits |= (uint64_t)best << (3 * i);
    }
  }
  for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

/* --- BC7, mode 6 (one subset, RGBA 7.7.7.7 + p-bit, 4-bit indices) --- */

static const int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct {
  uint8_t bytes[16];
  int pos;
} bitw_t;

static void put_bits(bitw_t *b, uint32_t v, int n) {
  for (int i = 0; i < n; i++, b->pos++)
    if (v >> i & 1) b->bytes[b->pos >> 3] |= (uint8_t)(1 << (b->pos & 7));
}

/* 7-bit values + p-bit closest to endpoint e. */
static void quant7(const float e[4], int q[4], int *p) {
  float best = 1e30f;
  for (int pb = 0; pb < 2; pb++) {
    int t[4];
    float err = 0;
    for (int c = 0; c < 4; c++) {
      int v = (int)floorf((e[c] - pb) / 2.0f + 0.5f);
      if (v < 0) v = 0;
      if (v > 127) v = 127;
      t[c] = v;
      float d = (float)(v * 2 + pb) - e[c];
      err += d * d;
    }
    if (err < best) {
      best = err;
      memcpy(q, t, sizeof(t));
      *p = pb;
    }
  }
}

static float bc7_try(const uint8_t px[16][4], const float e0[4], const float e1[4], int q0[4],
                     int q1[4], int *p0, int *p1, int idx[16]) {
  quant7(e0, q0, p0);
  quant7(e1, q1, p1);
  int a[4], b[4], pal[16][4];
  for (int c = 0; c < 4; c++) {
    a[c] = q0[c] * 2 + *p0;
    b[c] = q1[c] * 2 + *p1;
  }
  for (int k = 0; k < 16; k++)
    for (int c = 0; c < 4; c++) pal[k][c] = ((64 - kW4[k]) * a[c] + kW4[k] * b[c] + 32) >> 6;
  float total = 0;
  for (int i = 0; i < 16; i++) {
    int best = 0, bd = 1 << 30;
    for (int k = 0; k < 16; k++) {
      int d = 0;
      for (int c = 0; c < 4; c++) d += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
      if (d < bd) {
        bd = d;
        best = k;
      }
    }
    idx[i] = best;
    total += (float)bd;
  }
  return total;
}

static void bc7_block(const uint8_t px[16][4], uint8_t out[16]) {
  float e0[4], e1[4];
  axis_endpoints(px, 4, e0, e1);
  int q0[4], q1[4], p0, p1, idx[16];
  float err = bc7_try(px, e0, e1, q0, q1, &p0, &p1, idx);

  /* one least-squares pass on the endpoints, kept if it helps */
  float s00 = 0, s01 = 0, s11 = 0, r0[4] = {0}, r1[4] = {0};
  for (int i = 0; i < 16; i++) {
    float w = kW4[idx[i]] / 64.0f, v = 1 - w;
    s00 += v * v;
    s01 += v * w;
    s11 += w * w;
    for (int c = 0; c < 4; c++) {
      r0[c] += v * px[i][c];
      r1[c] += w * px[i][c];
    }
  }
  float det = s00 * s11 - s01 * s01;
  if (fabsf(det) > 1e-4f) {
    float n0[4], n1[4];
    for (int c = 0; c < 4; c++) {
      n0[c] = (r0[c] * s11 - r1[c] * s01) / det;
      n1[c] = (r1[c] * s00 - r0[c] * s01) / det;
      n0[c] = n0[c] < 0 ? 0 : n0[c] > 255 ? 255 : n0[c];
      n1[c] = n1[c] < 0 ? 0 : n1[c] > 255 ? 255 : n1[c];
    }
    int tq0[4], tq1[4], tp0, tp1, tidx[16];
    float e2 = bc7_try(px, n0, n1, tq0, tq1, &tp0, &tp1, tidx);
    if (e2 < err) {
      memcpy(q0, tq0, sizeof(q0));
      memcpy(q1, tq1, sizeof(q1));
      p0 = tp0;
      p1 = tp1;
      memcpy(idx, tidx, sizeof(idx));
    }
  }

  /* the first index is stored with 3 bits: its top bit must be 0 */
  if (idx[0] & 8) {
    int t[4];
    memcpy(t, q0, sizeof(t));
    memcpy(q0, q1, sizeof(t));
    memcpy(q1, t, sizeof(t));
    int tp = p0;
    p0 = p1;
    p1 = tp;
    for (int i = 0; i < 16; i++) idx[i] = 15 - idx[i];
  }

  bitw_t b;
  memset(&b, 0, sizeof(b));
  put_bits(&b, 1u << 6, 7); /* mode 6 */
  for (int c = 0; c < 4; c++) {
    put_bits(&b, (uint32_t)q0[c], 7);
    put_bits(&b, (uint32_t)q1[c], 7);
  }
  put_bits(&b, (uint32_t)p0, 1);
  put_bits(&b, (uint32_t)p1, 1);
  put_bits(&b, (uint32_t)idx[0], 3);
  for (int i = 1; i < 16; i++) put_bits(&b, (uint32_t)idx[i], 4);
  memcpy(out, b.bytes, 16);
}

/* ------------------------------------------------------------------------- */
/* DDS                                                                       */
/* ------------------------------------------------------------------------- */

static int block_bytes(unsigned dxgi) {
  switch (dxgi) {
    case DXGI_BC1: case DXGI_BC1_SRGB: return 8;
    case DXGI_BC3: case DXGI_BC3_SRGB:
    case DXGI_BC7: case DXGI_BC7_SRGB: return 16;
    default: return 0; /* uncompressed */
  }
}

static size_t level_size(unsigned dxgi, int w, int h) {
  int bb = block_bytes(dxgi);
  if (bb) return (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * (size_t)bb;
  return (size_t)w * h * 4;
}

static void encode_level(const uint8_t *img, int w, int h, unsigned dxgi, uint8_t *out) {
  int bb = block_bytes(dxgi);
  if (!bb) {
    int bgr = dxgi == DXGI_BGRA8 || dxgi == DXGI_BGRA8_SRGB;
    for (size_t i = 0; i < (size_t)w * h; i++) {
      out[i * 4 + 0] = img[i * 4 + (bgr ? 2 : 0)];
      out[i * 4 + 1] = img[i * 4 + 1];
      out[i * 4 + 2] = img[i * 4 + (bgr ? 0 : 2)];
      out[i * 4 + 3] = img[i * 4 + 3];
    }
    return;
  }
  uint8_t px[16][4];
  for (int by = 0; by < h; by += 4)
    for (int bx = 0; bx < w; bx += 4) {
      get_block(img, w, h, bx, by, px);
      if (dxgi == DXGI_BC1 || dxgi == DXGI_BC1_SRGB) {
        bc1_block(px, out);
      } else if (dxgi == DXGI_BC3 || dxgi == DXGI_BC3_SRGB) {
        bc4_alpha(px, out);
        bc1_block(px, out + 8);
      } else {
        bc7_block(px, out);
      }
      out += bb;
    }
}

/* Half-size copy (2x2 average). */
static uint8_t *half(const uint8_t *img, int w, int h, int *nw, int *nh) {
  *nw = w > 1 ? w / 2 : 1;
  *nh = h > 1 ? h / 2 : 1;
  uint8_t *o = malloc((size_t)*nw * *nh * 4);
  if (!o) return NULL;
  for (int y = 0; y < *nh; y++)
    for (int x = 0; x < *nw; x++)
      for (int c = 0; c < 4; c++) {
        int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
        int y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
        int s = img[((size_t)y0 * w + x0) * 4 + c] + img[((size_t)y0 * w + x1) * 4 + c] +
                img[((size_t)y1 * w + x0) * 4 + c] + img[((size_t)y1 * w + x1) * 4 + c];
        o[((size_t)y * *nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
      }
  return o;
}

static int encode_dds(uint8_t *img, const tex_fmt_t *f, unsigned char **out, size_t *out_len) {
  int max_mips = 1;
  for (int m = f->w > f->h ? f->w : f->h; m > 1; m >>= 1) max_mips++;
  int mips = f->mips < 1 ? 1 : f->mips > max_mips ? max_mips : f->mips;
  size_t hdr = f->legacy ? 128 : 148, total = hdr;
  for (int l = 0, w = f->w, h = f->h; l < mips; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
    total += level_size(f->dxgi, w, h);
  uint8_t *b = calloc(1, total);
  if (!b) return -1;

  int compressed = block_bytes(f->dxgi) != 0;
  memcpy(b, "DDS ", 4);
  wr32(b + 4, 124);
  wr32(b + 8, 0x1 | 0x2 | 0x4 | 0x1000 | (mips > 1 ? 0x20000 : 0) | (compressed ? 0x80000 : 0x8));
  wr32(b + 12, (uint32_t)f->h);
  wr32(b + 16, (uint32_t)f->w);
  wr32(b + 20, compressed ? (uint32_t)level_size(f->dxgi, f->w, f->h) : (uint32_t)f->w * 4);
  wr32(b + 28, (uint32_t)mips);
  wr32(b + 76, 32);
  if (!f->legacy) {
    wr32(b + 80, 0x4); /* FOURCC */
    memcpy(b + 84, "DX10", 4);
    wr32(b + 128, f->dxgi);
    wr32(b + 132, 3); /* TEXTURE2D */
    wr32(b + 140, 1); /* array size */
  } else if (compressed) {
    wr32(b + 80, 0x4);
    memcpy(b + 84, f->dxgi == DXGI_BC1 || f->dxgi == DXGI_BC1_SRGB ? "DXT1" : "DXT5", 4);
  } else {
    int bgr = f->dxgi == DXGI_BGRA8 || f->dxgi == DXGI_BGRA8_SRGB;
    wr32(b + 80, 0x40 | 0x1); /* RGB + alpha */
    wr32(b + 88, 32);
    wr32(b + 92, bgr ? 0x00ff0000u : 0x000000ffu);
    wr32(b + 96, 0x0000ff00u);
    wr32(b + 100, bgr ? 0x000000ffu : 0x00ff0000u);
    wr32(b + 104, 0xff000000u);
  }
  wr32(b + 108, 0x1000 | (mips > 1 ? 0x400000 | 0x8 : 0));

  uint8_t *dst = b + hdr, *cur = img;
  int w = f->w, h = f->h;
  for (int l = 0; l < mips; l++) {
    encode_level(cur, w, h, f->dxgi, dst);
    dst += level_size(f->dxgi, w, h);
    if (l + 1 < mips) {
      int nw, nh;
      uint8_t *next = half(cur, w, h, &nw, &nh);
      if (cur != img) free(cur);
      if (!next) {
        free(b);
        return -1;
      }
      cur = next;
      w = nw;
      h = nh;
    }
  }
  if (cur != img) free(cur);
  *out = b;
  *out_len = total;
  return 0;
}

int tex_encode(const unsigned char *img_data, size_t img_len, const tex_fmt_t *f,
               unsigned char **out, size_t *out_len) {
  if (!tex_supported(f) || !sane(f->w) || !sane(f->h)) return -1;
  uint8_t *img = icon_load_cover(img_data, img_len, f->w, f->h);
  if (!img) return -1;
  for (size_t i = 0; i < (size_t)f->w * f->h; i++) img[i * 4 + 3] = 255; /* backgrounds are solid */
  int rc;
  if (f->kind == TEX_DDS) {
    rc = encode_dds(img, f, out, out_len);
  } else if (f->kind == TEX_JPG) {
    rc = img_encode_jpg(img, f->w, f->h, 92, out, out_len);
  } else {
    for (size_t p = 0; p < (size_t)f->w * f->h; p++) memmove(img + p * 3, img + p * 4, 3);
    rc = img_encode_png(img, f->w, f->h, 3, out, out_len);
  }
  free(img);
  return rc;
}
