/* Pictures on USB drives, for folder artwork chosen on the PS5 itself
 * (the PS5 browser can't pick files, so Home Hider browses the drive). */
#pragma once

#include <stddef.h>

#include "strbuf.h"

#define USBPIC_MAX_BYTES (32u * 1024 * 1024)

/* local_dir: a folder on internal storage that is also searched, for
 * pictures copied over FTP (created if missing). */
void usbpics_init(const char *usb_fmt, const char *cache_dir, const char *local_dir);
/* JSON array: [{"path","name","folder","size"}...] of pictures found on USB
 * drives and in local_dir (top folder and up to four folders deep, so PS5
 * screenshot folders are included). */
int  usbpics_list_json(strbuf_t *sb);
/* 1 if path is a picture file on a USB drive or in local_dir (no "..",
 * known extension). */
int  usbpics_valid_path(const char *path);
/* Reads a USB picture into a malloc'd buffer. 0 on success. */
int  usbpics_read(const char *path, unsigned char **buf, size_t *len);
/* Square PNG preview (cached on internal storage). 0 on success. */
int  usbpics_thumb(const char *path, unsigned char **png, size_t *len);
