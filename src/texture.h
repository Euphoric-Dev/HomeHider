/* Home screen background pictures (pic0 / pic1).
 *
 * A new background is written in the same kind of file as the one it
 * replaces, so the home screen reads it the same way:
 *   - PNG or JPEG, at the original size
 *   - DDS with the original texture format and mip count: BC7, BC1 (DXT1),
 *     BC3 (DXT5) or uncompressed RGBA/BGRA
 * Pictures are cropped from the middle to the background's shape.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum { TEX_PNG = 1, TEX_JPG, TEX_DDS } tex_kind_t;

typedef struct {
  tex_kind_t kind;
  int      w, h;
  int      mips;     /* DDS: mip levels (1 = none) */
  unsigned dxgi;     /* DDS: DXGI format number */
  int      legacy;   /* DDS: old-style header without the DX10 part */
} tex_fmt_t;

#define TEX_BG_W 3840
#define TEX_BG_H 2160

/* Format of an existing background. path may end in "?query". Reads the
 * file's header when it can; otherwise guesses from the extension (DDS:
 * BC7, 3840x2160, no mips). Returns -1 for an unknown extension. */
int  tex_probe(const char *path, tex_fmt_t *f);

/* 1 if Home Hider can write this format. */
int  tex_supported(const tex_fmt_t *f);

/* Short text such as "DDS BC7 3840x2160, 12 mips", for logs. */
void tex_describe(const tex_fmt_t *f, char *out, size_t n);

/* File extension for the format ("png", "jpg", "dds"). */
const char *tex_ext(const tex_fmt_t *f);

/* Encodes an RGBA picture of any size into format f. *out is malloc'd. */
int  tex_encode(const unsigned char *img, size_t img_len, const tex_fmt_t *f,
                unsigned char **out, size_t *out_len);
