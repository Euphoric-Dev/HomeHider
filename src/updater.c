#include "updater.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hider.h"
#include "json.h"
#include "platform.h"
#include "sha256.h"

/* Kept in the binary so other builds (and this updater) can read our version. */
__attribute__((used)) static const char MARKER[] = "HOMEHIDER-ELF-VERSION=" HH_VERSION ";";
const char *upd_marker(void) { return MARKER; }

#define MIN_ELF   (16u * 1024)
#define MAX_COPIES 16

static struct {
  char data_dir[256], backup_dir[256], pldmgr_dir[256], usb_fmt[256], github_api[128];
  int loader_port;
} C;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_repo[128];

static struct {
  int busy;
  char phase[16];
  char message[320];
  volatile long long progress;

  int gh_checked, gh_ok, gh_newer, gh_has_digest;
  char gh_error[200], gh_version[64], gh_notes[1600], gh_page[300];
  char gh_asset_url[700], gh_asset_api[300], gh_asset_name[128], gh_digest[80];

  int usb_checked, usb_found, usb_newer;
  char usb_path[512], usb_version[32];
} S;

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void set_phase(const char *phase, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_phase(const char *phase, const char *fmt, ...) {
  pthread_mutex_lock(&g_mx);
  snprintf(S.phase, sizeof(S.phase), "%s", phase);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(S.message, sizeof(S.message), fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&g_mx);
}

static int parse_ver(const char *s, int v[4]) {
  memset(v, 0, 4 * sizeof(int));
  if (!s) return -1;
  while (*s == ' ') s++;
  if (*s == 'v' || *s == 'V') s++;
  if (!isdigit((unsigned char)*s)) return -1;
  for (int i = 0; i < 4; i++) {
    if (!isdigit((unsigned char)*s)) break;
    long n = 0;
    while (isdigit((unsigned char)*s) && n < 100000) n = n * 10 + (*s++ - '0');
    v[i] = (int)n;
    if (*s != '.') break;
    s++;
  }
  return 0;
}

int upd_version_cmp(const char *a, const char *b) {
  int x[4], y[4];
  parse_ver(a, x);
  parse_ver(b, y);
  for (int i = 0; i < 4; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

static int ends_with_ci(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static int contains_ci(const char *s, const char *needle) {
  size_t n = strlen(needle);
  for (; *s; s++)
    if (strncasecmp(s, needle, n) == 0) return 1;
  return 0;
}

static const unsigned char *mem_find(const unsigned char *h, size_t hl,
                                     const char *needle, size_t nl) {
  if (nl == 0 || hl < nl) return NULL;
  const unsigned char *p = h, *end = h + hl - nl + 1;
  while (p < end) {
    p = memchr(p, (unsigned char)needle[0], (size_t)(end - p));
    if (!p) return NULL;
    if (memcmp(p, needle, nl) == 0) return p;
    p++;
  }
  return NULL;
}

int upd_find_version(const unsigned char *buf, size_t len, char *ver, size_t versz) {
  static const char key[] = "HOMEHIDER-ELF-VERSION=";
  const size_t kl = sizeof(key) - 1;
  size_t off = 0;
  while (off < len) {
    const unsigned char *p = mem_find(buf + off, len - off, key, kl);
    if (!p) return -1;
    const unsigned char *v = p + kl;
    size_t n = 0;
    while (v + n < buf + len && n < 31 && (isdigit(v[n]) || v[n] == '.')) n++;
    if (n > 0 && v + n < buf + len && v[n] == ';' && isdigit(v[0])) {
      if (n >= versz) return -1;
      memcpy(ver, v, n);
      ver[n] = '\0';
      return 0;
    }
    off = (size_t)(p - buf) + 1;
  }
  return -1;
}

static int validate_elf(const unsigned char *b, size_t len, char *ver, size_t versz,
                        char *err, size_t errsz) {
  if (len < MIN_ELF) { snprintf(err, errsz, "The file is too small to be Home Hider."); return -1; }
  if (len > UPD_MAX_ELF) { snprintf(err, errsz, "The file is too large."); return -1; }
  if (memcmp(b, "\x7f" "ELF", 4) != 0 || b[4] != 2 || b[5] != 1) {
    snprintf(err, errsz, "That isn't a PS5 payload (.elf) file.");
    return -1;
  }
  if ((b[18] | (b[19] << 8)) != 0x3E) {
    snprintf(err, errsz, "That ELF isn't built for the PS5 (x86-64).");
    return -1;
  }
  if (upd_find_version(b, len, ver, versz)) {
    snprintf(err, errsz, "That .elf isn't a Home Hider build (no version marker).");
    return -1;
  }
  return 0;
}

static int read_file(const char *path, size_t max, unsigned char **out, size_t *len) {
  *out = NULL;
  *len = 0;
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > max)
    return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char *b = malloc((size_t)st.st_size);
  if (!b) { fclose(f); return -1; }
  size_t got = fread(b, 1, (size_t)st.st_size, f);
  fclose(f);
  if (got != (size_t)st.st_size) { free(b); return -1; }
  *out = b;
  *len = got;
  return 0;
}

static int write_atomic(const char *path, const unsigned char *b, size_t len) {
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.hhnew", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, b + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      unlink(tmp);
      return -1;
    }
    off += (size_t)n;
  }
  if (fsync(fd) || close(fd)) {
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  chmod(path, 0777);
  return 0;
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ------------------------------------------------------------------------- */
/* finding installed copies                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
  char path[512];
  char version[32];  /* "" for builds older than the marker (v0.1/v0.2) */
} copy_t;

static void scan_copies(const char *dir, int depth, copy_t *out, int *n) {
  if (depth > 4 || *n >= MAX_COPIES) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && *n < MAX_COPIES) {
    if (e->d_name[0] == '.') continue;
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) {
      scan_copies(p, depth + 1, out, n);
      continue;
    }
    if (!S_ISREG(st.st_mode) || !(ends_with_ci(e->d_name, ".elf") || ends_with_ci(e->d_name, ".bin")))
      continue;
    unsigned char *b;
    size_t len;
    if (read_file(p, UPD_MAX_ELF, &b, &len)) continue;
    char ver[32] = "";
    int ours = len > 4 && memcmp(b, "\x7f" "ELF", 4) == 0 &&
               (upd_find_version(b, len, ver, sizeof(ver)) == 0 ||
                mem_find(b, len, "/data/homehider", 15) != NULL);
    free(b);
    if (!ours) continue;
    snprintf(out[*n].path, sizeof(out[*n].path), "%s", p);
    snprintf(out[*n].version, sizeof(out[*n].version), "%s", ver);
    (*n)++;
  }
  closedir(d);
}

static int install_bytes(const unsigned char *b, size_t len, const char *ver,
                         char *msg, size_t msgsz) {
  copy_t copies[MAX_COPIES];
  int n = 0, written = 0, failed = 0;
  scan_copies(C.pldmgr_dir, 0, copies, &n);
  mkdir(C.backup_dir, 0777);

  for (int i = 0; i < n; i++) {
    char bk[600];
    snprintf(bk, sizeof(bk), "%s/HomeHider-%s.elf", C.backup_dir,
             copies[i].version[0] ? copies[i].version : "previous");
    struct stat st;
    if (stat(bk, &st) != 0) {
      unsigned char *old;
      size_t ol;
      if (read_file(copies[i].path, UPD_MAX_ELF, &old, &ol) == 0) {
        write_atomic(bk, old, ol);
        free(old);
      }
    }
    if (write_atomic(copies[i].path, b, len) == 0) written++;
    else failed++;
  }
  if (n == 0 && is_dir(C.pldmgr_dir)) {
    char p[512];
    snprintf(p, sizeof(p), "%s/HomeHider.elf", C.pldmgr_dir);
    if (write_atomic(p, b, len) == 0) written++;
    else failed++;
  }
  char own[512];
  snprintf(own, sizeof(own), "%s/HomeHider.elf", C.data_dir);
  int own_ok = write_atomic(own, b, len) == 0;

  if (written == 0 && !own_ok) {
    snprintf(msg, msgsz, "Couldn't write the new version anywhere.");
    return -1;
  }
  if (failed)
    snprintf(msg, msgsz, "Installed v%s to %d place(s); %d copy could not be replaced.", ver,
             written, failed);
  else if (written)
    snprintf(msg, msgsz, "Installed v%s in Payload Manager (%d cop%s).", ver, written,
             written == 1 ? "y" : "ies");
  else
    snprintf(msg, msgsz, "Installed v%s to %.200s (Payload Manager folder not found).", ver, own);
  return 0;
}

/* Hands the new build to the ELF loader. The new instance then asks this
 * one to quit and takes over the port. */
static int relaunch(const unsigned char *b, size_t len, const char *ver) {
  char flag[300];
  snprintf(flag, sizeof(flag), "%s/.updated", C.data_dir);
  FILE *f = fopen(flag, "w");
  if (f) {
    fprintf(f, "%s\n", ver);
    fclose(f);
  }
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) goto fail;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)C.loader_port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof(a))) {
    close(s);
    goto fail;
  }
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(s, b + off, len - off, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      close(s);
      goto fail;
    }
    off += (size_t)n;
  }
  close(s);
  return 0;
fail:
  unlink(flag);
  return -1;
}

typedef struct {
  unsigned char *buf;
  size_t len;
  char ver[32];
} relaunch_job_t;

static void *relaunch_later(void *arg) {
  relaunch_job_t *j = arg;
  usleep(400000); /* let the HTTP reply reach the browser first */
  if (relaunch(j->buf, j->len, j->ver))
    set_phase("done", "Installed v%s. Quit Home Hider and start it again from Payload Manager.", j->ver);
  free(j->buf);
  free(j);
  return NULL;
}

/* Installs + restarts. Takes ownership of b. */
static int install_and_restart(unsigned char *b, size_t len, const char *ver, int async_restart) {
  char msg[300];
  set_phase("installing", "Installing v%s…", ver);
  if (install_bytes(b, len, ver, msg, sizeof(msg))) {
    set_phase("error", "%s", msg);
    free(b);
    return -1;
  }
  set_phase("restarting", "%s Restarting Home Hider…", msg);
  if (async_restart) {
    relaunch_job_t *j = malloc(sizeof(*j));
    pthread_t t;
    if (j) {
      j->buf = b;
      j->len = len;
      snprintf(j->ver, sizeof(j->ver), "%s", ver);
      if (pthread_create(&t, NULL, relaunch_later, j) == 0) {
        pthread_detach(t);
        return 0;
      }
      free(j);
    }
  }
  int rc = relaunch(b, len, ver);
  free(b);
  if (rc)
    set_phase("done", "%s Quit Home Hider and start it again from Payload Manager.", msg);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* checks                                                                    */
/* ------------------------------------------------------------------------- */

static void check_github(void) {
  char repo[128];
  pthread_mutex_lock(&g_mx);
  snprintf(repo, sizeof(repo), "%s", g_repo);
  S.gh_checked = 1;
  S.gh_ok = S.gh_newer = S.gh_has_digest = 0;
  S.gh_error[0] = S.gh_version[0] = S.gh_notes[0] = S.gh_page[0] = '\0';
  S.gh_asset_url[0] = S.gh_asset_api[0] = S.gh_asset_name[0] = S.gh_digest[0] = '\0';
  pthread_mutex_unlock(&g_mx);

  char err[200] = "";
  if (!repo[0]) {
    pthread_mutex_lock(&g_mx);
    snprintf(S.gh_error, sizeof(S.gh_error), "No GitHub repository set.");
    pthread_mutex_unlock(&g_mx);
    return;
  }

  set_phase("checking", "Checking GitHub (%s)…", repo);
  char url[400];
  snprintf(url, sizeof(url), "%s/repos/%s/releases/latest", C.github_api, repo);
  unsigned char *b = NULL;
  size_t len = 0;
  int status = plat_http_get(url, &b, &len, 2u * 1024 * 1024, NULL);
  if (status != 200) {
    if (status == 404) snprintf(err, sizeof(err), "No published release found in %s.", repo);
    else if (status < 0) snprintf(err, sizeof(err), "Couldn't reach GitHub (error %d).", status);
    else snprintf(err, sizeof(err), "GitHub answered HTTP %d.", status);
    free(b);
    pthread_mutex_lock(&g_mx);
    snprintf(S.gh_error, sizeof(S.gh_error), "%s", err);
    pthread_mutex_unlock(&g_mx);
    return;
  }

  int max = (int)(len / 2 + 16);
  json_tok_t *t = malloc((size_t)max * sizeof(json_tok_t));
  int n = t ? json_parse((const char *)b, len, t, max) : -1;
  const char *js = (const char *)b;
  char tag[64] = "", notes[1600] = "", page[300] = "";
  char best_name[128] = "", best_url[700] = "", best_api[300] = "", best_digest[80] = "";
  int best_score = -1;
  if (n > 0 && t[0].type == JSON_OBJ) {
    json_str(js, t, json_get(js, t, n, 0, "tag_name"), tag, sizeof(tag));
    json_str(js, t, json_get(js, t, n, 0, "body"), notes, sizeof(notes));
    json_str(js, t, json_get(js, t, n, 0, "html_url"), page, sizeof(page));
    int assets = json_get(js, t, n, 0, "assets");
    for (int i = 0;; i++) {
      int a = json_at(t, n, assets, i);
      if (a < 0) break;
      char name[128] = "";
      if (json_str(js, t, json_get(js, t, n, a, "name"), name, sizeof(name)) != 0) continue;
      if (!ends_with_ci(name, ".elf")) continue;
      int score = contains_ci(name, "homehider") ? 2 : 1;
      if (score <= best_score) continue;
      char u[700] = "", api[300] = "", dg[80] = "";
      if (json_str(js, t, json_get(js, t, n, a, "browser_download_url"), u, sizeof(u)) != 0) continue;
      json_str(js, t, json_get(js, t, n, a, "url"), api, sizeof(api));
      json_str(js, t, json_get(js, t, n, a, "digest"), dg, sizeof(dg));
      best_score = score;
      snprintf(best_name, sizeof(best_name), "%s", name);
      snprintf(best_url, sizeof(best_url), "%s", u);
      snprintf(best_api, sizeof(best_api), "%s", api);
      snprintf(best_digest, sizeof(best_digest), "%s", dg);
    }
  }
  free(t);
  free(b);

  pthread_mutex_lock(&g_mx);
  if (n <= 0) {
    snprintf(S.gh_error, sizeof(S.gh_error), "GitHub sent a reply Home Hider couldn't read.");
  } else if (!tag[0]) {
    snprintf(S.gh_error, sizeof(S.gh_error), "The latest release has no version tag.");
  } else if (best_score < 0) {
    snprintf(S.gh_error, sizeof(S.gh_error), "Release %s has no .elf file attached.", tag);
    snprintf(S.gh_version, sizeof(S.gh_version), "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
  } else {
    S.gh_ok = 1;
    snprintf(S.gh_version, sizeof(S.gh_version), "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
    snprintf(S.gh_notes, sizeof(S.gh_notes), "%s", notes);
    snprintf(S.gh_page, sizeof(S.gh_page), "%s", page);
    snprintf(S.gh_asset_name, sizeof(S.gh_asset_name), "%s", best_name);
    snprintf(S.gh_asset_url, sizeof(S.gh_asset_url), "%s", best_url);
    snprintf(S.gh_asset_api, sizeof(S.gh_asset_api), "%s", best_api);
    if (strncasecmp(best_digest, "sha256:", 7) == 0 && strlen(best_digest) == 71) {
      snprintf(S.gh_digest, sizeof(S.gh_digest), "%s", best_digest + 7);
      S.gh_has_digest = 1;
    }
    S.gh_newer = upd_version_cmp(S.gh_version, HH_VERSION) > 0;
  }
  pthread_mutex_unlock(&g_mx);
}

/* USB files already inspected (path + size + mtime -> version or ""), so
 * repeated checks don't re-read every payload on the drive. */
typedef struct {
  char path[512];
  long long size, mtime;
  char version[32];
} usb_seen_t;
static usb_seen_t g_seen[64];
static int g_seen_n, g_seen_next;

static const char *usb_version_of(const char *p) {
  struct stat st;
  if (stat(p, &st) || !S_ISREG(st.st_mode)) return "";
  for (int i = 0; i < g_seen_n; i++)
    if (!strcmp(g_seen[i].path, p) && g_seen[i].size == (long long)st.st_size &&
        g_seen[i].mtime == (long long)st.st_mtime)
      return g_seen[i].version;
  usb_seen_t *e = &g_seen[g_seen_next];
  g_seen_next = (g_seen_next + 1) % 64;
  if (g_seen_n < 64) g_seen_n++;
  snprintf(e->path, sizeof(e->path), "%s", p);
  e->size = (long long)st.st_size;
  e->mtime = (long long)st.st_mtime;
  e->version[0] = '\0';
  unsigned char *b;
  size_t len;
  if (read_file(p, UPD_MAX_ELF, &b, &len) == 0) {
    char ver[32], err[160];
    if (validate_elf(b, len, ver, sizeof(ver), err, sizeof(err)) == 0)
      snprintf(e->version, sizeof(e->version), "%s", ver);
    free(b);
  }
  return e->version;
}

static void check_usb_dir(const char *dir, char *best_path, size_t bp, char *best_ver, size_t bv) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.' || !ends_with_ci(e->d_name, ".elf")) continue;
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    const char *ver = usb_version_of(p);
    if (ver[0] && (!best_ver[0] || upd_version_cmp(ver, best_ver) > 0)) {
      snprintf(best_path, bp, "%s", p);
      snprintf(best_ver, bv, "%s", ver);
    }
  }
  closedir(d);
}

static void check_usb(void) {
  char best_path[512] = "", best_ver[32] = "";
  for (int i = 0; i < 8; i++) {
    char root[260];
    if ((size_t)snprintf(root, sizeof(root), C.usb_fmt, i) >= sizeof(root) || !is_dir(root)) continue;
    check_usb_dir(root, best_path, sizeof(best_path), best_ver, sizeof(best_ver));
    DIR *d = opendir(root);
    if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (strcasecmp(e->d_name, "homehider") && strcasecmp(e->d_name, "payloads") &&
          strcasecmp(e->d_name, "ps5"))
        continue;
      char sub[540];
      snprintf(sub, sizeof(sub), "%s/%s", root, e->d_name);
      if (is_dir(sub)) check_usb_dir(sub, best_path, sizeof(best_path), best_ver, sizeof(best_ver));
    }
    closedir(d);
  }
  pthread_mutex_lock(&g_mx);
  S.usb_checked = 1;
  S.usb_found = best_ver[0] != '\0';
  snprintf(S.usb_path, sizeof(S.usb_path), "%s", best_path);
  snprintf(S.usb_version, sizeof(S.usb_version), "%s", best_ver);
  S.usb_newer = S.usb_found && upd_version_cmp(best_ver, HH_VERSION) > 0;
  pthread_mutex_unlock(&g_mx);
}

/* ------------------------------------------------------------------------- */
/* jobs                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
  int install;
  int github, usb, notify;
  char source[16];
} job_t;

static void run_install(const char *source) {
  unsigned char *b = NULL;
  size_t len = 0;
  char ver[32], err[200];

  if (!strcmp(source, "github")) {
    char url[700], digest[80];
    int has_digest;
    pthread_mutex_lock(&g_mx);
    int ok = S.gh_ok && S.gh_newer;
    snprintf(url, sizeof(url), "%s", S.gh_asset_url);
    snprintf(digest, sizeof(digest), "%s", S.gh_digest);
    has_digest = S.gh_has_digest;
    S.progress = 0;
    pthread_mutex_unlock(&g_mx);
    if (!ok) {
      set_phase("error", "No newer GitHub release to install. Check for updates first.");
      return;
    }
    set_phase("downloading", "Downloading from GitHub…");
    int status = plat_http_get(url, &b, &len, UPD_MAX_ELF, &S.progress);
    if (status != 200) {
      free(b);
      set_phase("error", "Download failed (%d).", status);
      return;
    }
    if (has_digest) {
      char hex[65];
      sha256_hex(b, len, hex);
      if (strcasecmp(hex, digest) != 0) {
        free(b);
        set_phase("error", "The download didn't match GitHub's checksum, so it wasn't installed.");
        return;
      }
    }
  } else if (!strcmp(source, "usb")) {
    char path[512];
    pthread_mutex_lock(&g_mx);
    int ok = S.usb_found && S.usb_newer;
    snprintf(path, sizeof(path), "%s", S.usb_path);
    pthread_mutex_unlock(&g_mx);
    if (!ok || read_file(path, UPD_MAX_ELF, &b, &len)) {
      set_phase("error", "No newer Home Hider found on USB.");
      return;
    }
  } else {
    set_phase("error", "Unknown update source.");
    return;
  }

  if (validate_elf(b, len, ver, sizeof(ver), err, sizeof(err))) {
    free(b);
    set_phase("error", "%s", err);
    return;
  }
  if (upd_version_cmp(ver, HH_VERSION) <= 0) {
    free(b);
    set_phase("error", "That file is v%s, which isn't newer than v%s.", ver, HH_VERSION);
    return;
  }
  install_and_restart(b, len, ver, 0);
}

static void *worker(void *arg) {
  job_t j = *(job_t *)arg;
  free(arg);
  if (j.install) {
    run_install(j.source);
  } else {
    if (j.usb) check_usb();
    if (j.github) check_github();
    pthread_mutex_lock(&g_mx);
    int gh = S.gh_newer, usb = S.usb_newer;
    char gv[64], uv[32];
    snprintf(gv, sizeof(gv), "%s", S.gh_version);
    snprintf(uv, sizeof(uv), "%s", S.usb_version);
    pthread_mutex_unlock(&g_mx);
    if (gh || usb) {
      const char *v = gh && (!usb || upd_version_cmp(gv, uv) >= 0) ? gv : uv;
      set_phase("available", "Home Hider v%s is available.", v);
      if (j.notify)
        plat_notify("Home Hider v%s is available%s.\nOpen the menu to install it.", v,
                    gh ? " on GitHub" : " on USB");
    } else {
      set_phase("idle", "You have the latest version (v%s).", HH_VERSION);
    }
  }
  pthread_mutex_lock(&g_mx);
  S.busy = 0;
  pthread_mutex_unlock(&g_mx);
  return NULL;
}

static int start_job(const job_t *src) {
  pthread_mutex_lock(&g_mx);
  if (S.busy) {
    pthread_mutex_unlock(&g_mx);
    return -1;
  }
  S.busy = 1;
  pthread_mutex_unlock(&g_mx);
  job_t *j = malloc(sizeof(*j));
  pthread_t t;
  if (j) {
    *j = *src;
    if (pthread_create(&t, NULL, worker, j) == 0) {
      pthread_detach(t);
      return 0;
    }
    free(j);
  }
  pthread_mutex_lock(&g_mx);
  S.busy = 0;
  pthread_mutex_unlock(&g_mx);
  return -1;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

void upd_init(const upd_config_t *cfg) {
  snprintf(C.data_dir, sizeof(C.data_dir), "%s", cfg->data_dir);
  snprintf(C.backup_dir, sizeof(C.backup_dir), "%s", cfg->backup_dir);
  snprintf(C.pldmgr_dir, sizeof(C.pldmgr_dir), "%s", cfg->pldmgr_dir);
  snprintf(C.usb_fmt, sizeof(C.usb_fmt), "%s", cfg->usb_fmt);
  snprintf(C.github_api, sizeof(C.github_api), "%s", cfg->github_api);
  C.loader_port = cfg->loader_port;
  snprintf(S.phase, sizeof(S.phase), "idle");
}

int upd_valid_repo(const char *r) {
  if (!r) return 0;
  if (!r[0]) return 1; /* empty = not set */
  size_t n = strlen(r);
  if (n > 100) return 0;
  const char *slash = strchr(r, '/');
  if (!slash || slash == r || !slash[1] || strchr(slash + 1, '/')) return 0;
  for (const char *p = r; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.' && *p != '/')
      return 0;
  return strstr(r, "..") == NULL;
}

void upd_set_repo(const char *repo) {
  pthread_mutex_lock(&g_mx);
  snprintf(g_repo, sizeof(g_repo), "%s", upd_valid_repo(repo) ? repo : "");
  pthread_mutex_unlock(&g_mx);
}

int upd_start_check(int github, int usb, int notify_if_found) {
  job_t j = {0, github, usb, notify_if_found, ""};
  return start_job(&j);
}

int upd_start_install(const char *source) {
  job_t j = {1, 0, 0, 0, ""};
  snprintf(j.source, sizeof(j.source), "%s", source);
  return start_job(&j);
}

int upd_install_upload(const unsigned char *buf, size_t len, int allow_older,
                       char *msg, size_t msgsz) {
  char ver[32];
  if (validate_elf(buf, len, ver, sizeof(ver), msg, msgsz)) return -1;
  int cmp = upd_version_cmp(ver, HH_VERSION);
  if (cmp <= 0 && !allow_older) {
    snprintf(msg, msgsz, "That file is v%s and you're running v%s. Turn on \"Allow older "
             "versions\" to install it anyway.", ver, HH_VERSION);
    return -1;
  }
  pthread_mutex_lock(&g_mx);
  if (S.busy) {
    pthread_mutex_unlock(&g_mx);
    snprintf(msg, msgsz, "An update is already running.");
    return -1;
  }
  S.busy = 1;
  pthread_mutex_unlock(&g_mx);

  unsigned char *copy = malloc(len);
  int rc = -1;
  if (!copy) {
    snprintf(msg, msgsz, "Out of memory.");
  } else {
    memcpy(copy, buf, len);
    rc = install_and_restart(copy, len, ver, 1);
    pthread_mutex_lock(&g_mx);
    snprintf(msg, msgsz, "%s", S.message);
    pthread_mutex_unlock(&g_mx);
  }
  pthread_mutex_lock(&g_mx);
  S.busy = 0;
  pthread_mutex_unlock(&g_mx);
  return rc;
}

void upd_status_json(strbuf_t *sb) {
  pthread_mutex_lock(&g_mx);
  sb_printf(sb, "{\"ok\":true,\"current\":\"%s\",\"repo\":", HH_VERSION);
  sb_json_str(sb, g_repo);
  sb_printf(sb, ",\"busy\":%s,\"phase\":", S.busy ? "true" : "false");
  sb_json_str(sb, S.phase);
  sb_puts(sb, ",\"message\":");
  sb_json_str(sb, S.message);
  sb_printf(sb, ",\"progress\":%lld", (long long)S.progress);
  sb_printf(sb, ",\"github\":{\"checked\":%s,\"ok\":%s,\"newer\":%s,\"digest\":%s,\"error\":",
            S.gh_checked ? "true" : "false", S.gh_ok ? "true" : "false",
            S.gh_newer ? "true" : "false", S.gh_has_digest ? "true" : "false");
  sb_json_str(sb, S.gh_error[0] ? S.gh_error : NULL);
  sb_puts(sb, ",\"version\":");
  sb_json_str(sb, S.gh_version);
  sb_puts(sb, ",\"notes\":");
  sb_json_str(sb, S.gh_notes);
  sb_puts(sb, ",\"page\":");
  sb_json_str(sb, S.gh_page);
  sb_puts(sb, ",\"asset\":");
  sb_json_str(sb, S.gh_asset_name);
  sb_puts(sb, ",\"asset_api\":");
  sb_json_str(sb, S.gh_asset_api);
  sb_printf(sb, "},\"usb\":{\"checked\":%s,\"found\":%s,\"newer\":%s,\"version\":",
            S.usb_checked ? "true" : "false", S.usb_found ? "true" : "false",
            S.usb_newer ? "true" : "false");
  sb_json_str(sb, S.usb_version);
  sb_puts(sb, ",\"path\":");
  sb_json_str(sb, S.usb_path);
  sb_puts(sb, "}}");
  pthread_mutex_unlock(&g_mx);
}
