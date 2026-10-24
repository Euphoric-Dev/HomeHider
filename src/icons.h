/* Folder tile artwork: a PS4-style 2x2 collage of the games inside, or a
 * custom picture. All output is a 512x512 PNG. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ICON_SIZE 512

/* Known colour names: blue, purple, red, orange, green, teal, gray, and
 * none (no background: the picture keeps its see-through parts).
 * Returns 1 if name is one of them. */
int  icon_valid_color(const char *name);

/* Builds a collage from up to four image files (png/jpg). Missing or
 * unreadable files leave an empty slot. *png is malloc'd. 0 on success. */
int  icon_collage(const char *const *paths, int n, const char *color,
                  unsigned char **png, size_t *png_len);

/* Center-crops and scales a picture (PNG, JPEG, BMP, GIF first frame, TGA;
 * JPEG EXIF rotation honoured) to a size x size PNG. Transparent parts are
 * put on a dark background. */
int  icon_square(const unsigned char *data, size_t len, int size,
                 unsigned char **png, size_t *png_len);
/* A ICON_SIZE picture. bg = colour name to put transparent parts on, or
 * NULL to keep transparency (flatten later with icon_flatten). */
int  icon_from_image(const unsigned char *data, size_t len, const char *bg,
                     unsigned char **png, size_t *png_len);
/* Puts a transparent square PNG on a colour (NULL: dark tile colour).
 * Pictures without transparency are copied as they are. */
int  icon_flatten(const unsigned char *png, size_t len, const char *bg,
                  unsigned char **out, size_t *out_len);

/* Loads a picture as RGBA (EXIF rotation honoured), cropped to the w:h shape
 * from the middle and scaled to w x h. malloc'd; NULL if unreadable. */
uint8_t *icon_load_cover(const unsigned char *data, size_t len, int w, int h);

/* PNG (comp 3 = RGB, 4 = RGBA) and JPEG (from RGBA) encoders. 0 on success. */
int  img_encode_png(const uint8_t *px, int w, int h, int comp, unsigned char **png, size_t *len);
int  img_encode_jpg(const uint8_t *rgba, int w, int h, int quality, unsigned char **jpg, size_t *len);
