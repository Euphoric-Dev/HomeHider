#include "usbpics.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "icons.h"
#include "sha256.h"

#define MAX_PICS    400
#define MAX_VISITS  6000  /* directory entries looked at per scan */
#define MAX_DEPTH   4  /* reaches PS5/CREATE/Screenshots/<game>/ */
#define THUMB_SIZE  192

static char g_fmt[256] = "/mnt/usb%d";
static char g_cache[256];
static char g_local[256]; /* pictures folder on internal storage (for FTP) */

/* previews are small, but don't let them pile up forever */
static void prune_cache(void) {
  DIR *d = opendir(g_cache);
  if (!d) return;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) n += e->d_name[0] != '.';
  if (n > 600) {
    rewinddir(d);
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char p[600];
      if ((size_t)snprintf(p, sizeof(p), "%s/%s", g_cache, e->d_name) < sizeof(p)) unlink(p);
    }
  }
  closedir(d);
}

void usbpics_init(const char *usb_fmt, const char *cache_dir, const char *local_dir) {
  snprintf(g_fmt, sizeof(g_fmt), "%s", usb_fmt);
  snprintf(g_cache, sizeof(g_cache), "%s", cache_dir);
  snprintf(g_local, sizeof(g_local), "%s", local_dir ? local_dir : "");
  if (g_local[0]) mkdir(g_local, 0777);
  prune_cache();
}

/* root i: 0..7 = USB drives, 8 = internal pictures folder */
#define N_ROOTS 9
static int root_path(int i, char *out, size_t n) {
  if (i < 8) {
    if ((size_t)snprintf(out, n, g_fmt, i) >= n) out[0] = '\0';
  }
  else snprintf(out, n, "%s", g_local);
  return out[0] ? 0 : -1;
}

static int is_picture_name(const char *name) {
  static const char *const ext[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga"};
  if (name[0] == '.') return 0; /* "._x.jpg" files macOS leaves behind */
  const char *dot = strrchr(name, '.');
  if (!dot) return 0;
  for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
    if (!strcasecmp(dot, ext[i])) return 1;
  return 0;
}

static int skip_dir(const char *name) {
  return name[0] == '.' || name[0] == '$' || !strcasecmp(name, "System Volume Information");
}

int usbpics_valid_path(const char *path) {
  if (!path || strstr(path, "..") || strstr(path, "//")) return 0;
  const char *slash = strrchr(path, '/');
  if (!slash || !is_picture_name(slash + 1)) return 0;
  for (int i = 0; i < N_ROOTS; i++) {
    char root[256];
    if (root_path(i, root, sizeof(root))) continue;
    size_t n = strlen(root);
    if (strncmp(path, root, n) == 0 && path[n] == '/') return 1;
  }
  return 0;
}

typedef struct {
  strbuf_t *sb;
  int count, visits;
  size_t root_len;
  const char *label; /* shown for pictures in the top folder of this root */
} scan_t;

static void scan_dir(scan_t *s, const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && s->count < MAX_PICS && s->visits < MAX_VISITS) {
    s->visits++;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char p[768];
    if ((size_t)snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= sizeof(p)) continue;
    struct stat st;
    if (stat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < MAX_DEPTH && !skip_dir(e->d_name)) scan_dir(s, p, depth + 1);
      continue;
    }
    if (!S_ISREG(st.st_mode) || !is_picture_name(e->d_name) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > USBPIC_MAX_BYTES)
      continue;
    sb_puts(s->sb, s->count ? ",{\"path\":" : "{\"path\":");
    sb_json_str(s->sb, p);
    sb_puts(s->sb, ",\"name\":");
    sb_json_str(s->sb, e->d_name);
    sb_puts(s->sb, ",\"folder\":");
    if (depth == 0) {
      sb_json_str(s->sb, s->label);
    } else if (s->label[0]) {
      char where[600];
      snprintf(where, sizeof(where), "%s/%s", s->label, dir + s->root_len + 1);
      sb_json_str(s->sb, where);
    } else {
      sb_json_str(s->sb, dir + s->root_len + 1);
    }
    sb_printf(s->sb, ",\"size\":%lld}", (long long)st.st_size);
    s->count++;
  }
  closedir(d);
}

int usbpics_list_json(strbuf_t *sb) {
  sb_puts(sb, "[");
  scan_t s = {sb, 0, 0, 0, ""};
  for (int i = 0; i < N_ROOTS; i++) {
    char root[256];
    struct stat st;
    if (root_path(i, root, sizeof(root)) || stat(root, &st) || !S_ISDIR(st.st_mode)) continue;
    s.root_len = strlen(root);
    s.label = i < 8 ? "" : "PS5 storage";
    scan_dir(&s, root, 0);
  }
  sb_puts(sb, "]");
  return sb->oom ? -1 : 0;
}

int usbpics_read(const char *path, unsigned char **buf, size_t *len) {
  *buf = NULL;
  *len = 0;
  if (!usbpics_valid_path(path)) return -1;
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      (unsigned long long)st.st_size > USBPIC_MAX_BYTES)
    return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char *b = malloc((size_t)st.st_size);
  if (!b) {
    fclose(f);
    return -1;
  }
  size_t n = fread(b, 1, (size_t)st.st_size, f);
  fclose(f);
  if (n != (size_t)st.st_size) {
    free(b);
    return -1;
  }
  *buf = b;
  *len = n;
  return 0;
}

int usbpics_thumb(const char *path, unsigned char **png, size_t *len) {
  *png = NULL;
  *len = 0;
  if (!usbpics_valid_path(path)) return -1;
  struct stat st;
  if (stat(path, &st)) return -1;

  /* cache key: path + size + modification time */
  char key[900], hex[65], cpath[400];
  snprintf(key, sizeof(key), "%s|%lld|%lld", path, (long long)st.st_size, (long long)st.st_mtime);
  sha256_hex(key, strlen(key), hex);
  snprintf(cpath, sizeof(cpath), "%s/%.32s.png", g_cache, hex);
  FILE *f = fopen(cpath, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 4 * 1024 * 1024 && (*png = malloc((size_t)sz))) {
      *len = fread(*png, 1, (size_t)sz, f);
      fclose(f);
      if (*len == (size_t)sz) return 0;
      free(*png);
      *png = NULL;
    } else {
      fclose(f);
    }
  }

  unsigned char *img;
  size_t il;
  if (usbpics_read(path, &img, &il)) return -1;
  int rc = icon_square(img, il, THUMB_SIZE, png, len);
  free(img);
  if (rc) return -1;
  mkdir(g_cache, 0777);
  char tmp[420];
  snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
  f = fopen(tmp, "wb");
  if (f) {
    int ok = fwrite(*png, 1, *len, f) == *len;
    if (fclose(f) == 0 && ok) rename(tmp, cpath);
    else unlink(tmp);
  }
  return 0;
}
