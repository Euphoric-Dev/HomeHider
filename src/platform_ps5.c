#include "platform.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

#include <ps5/kernel.h>

/* Classic notification request (works across firmware versions). */
typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *req,
                                     size_t size, int blocking);
int sceUserServiceInitialize(void *);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
int sceUserServiceGetForegroundUser(int *user_id);

typedef struct {
  uint32_t sz;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} lnc_app_param_t;

int sceLncUtilLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
int sceLncUtilKillApp(uint32_t app_id);

/* system HTTPS stack (same calls as the SDK's http2_get sample) */
int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2SetAutoRedirect(int, int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2ReadData(int, void *, size_t);
int sceHttp2GetAllResponseHeaders(int, char **, size_t *);

/* app installer (same calls Payload Manager uses for its launcher tile) */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

/* Launcher calls that are looked up at run time: a payload that links a
 * function the firmware doesn't export fails to load at all. */
static void *sysfn(const char *name) {
  static uint32_t handle;
  static int have = -1;
  if (have < 0) have = kernel_dynlib_handle(-1, "libSceSystemService.sprx", &handle) == 0;
  if (!have) return NULL;
  intptr_t p = kernel_dynlib_dlsym(-1, handle, name);
  return (p == 0 || p == -1) ? NULL : (void *)p;
}

static int valid_app_id(int id) {
  return id != 0 && id != -1 && !((uint32_t)id & 0x80000000u);
}

void plat_init(void) {
  /* needed by sceSystemServiceLaunchWebBrowser; harmless if already done */
  sceUserServiceInitialize(0);
  /* the launcher wants this before LaunchApp/KillApp; "already done" is fine */
  int (*lnc_init)(void) = sysfn("sceLncUtilInitialize");
  if (lnc_init) lnc_init();
}

void plat_notify(const char *fmt, ...) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  printf("[HomeHider] %s\n", req.message);
}

int plat_launch_browser(const char *url) {
  return sceSystemServiceLaunchWebBrowser(url, 0) == 0 ? 0 : -1;
}

int plat_launch_app(const char *title_id, uint32_t *app_id) {
  *app_id = 0;
  int user = 0;
  int rc = sceUserServiceGetForegroundUser(&user);
  if (rc != 0) return rc;
  lnc_app_param_t p;
  memset(&p, 0, sizeof(p));
  p.sz = sizeof(p);
  p.user_id = (uint32_t)user;
  /* success returns the new app's id (0x6xxxxxxx); errors have the top bit set */
  rc = sceLncUtilLaunchApp(title_id, NULL, &p);
  if ((uint32_t)rc & 0x80000000u) return rc;
  *app_id = (uint32_t)rc;
  return 0;
}

int plat_app_id(const char *title_id, uint32_t *app_id) {
  int (*get)(const char *) = sysfn("sceLncUtilGetAppId");
  if (!get) return -1;
  int id = get(title_id);
  if (!valid_app_id(id)) return -1;
  *app_id = (uint32_t)id;
  return 0;
}

int plat_running_game(char *title_id, size_t n, uint32_t *app_id) {
  int (*big)(void) = sysfn("sceLncUtilGetAppIdOfRunningBigApp");
  if (!big) big = sysfn("sceSystemServiceGetAppIdOfRunningBigApp");
  int (*title)(uint32_t, char *) = sysfn("sceLncUtilGetAppTitleId");
  if (!big) return -1;
  int id = big();
  if (!valid_app_id(id)) return -1;
  char t[64];
  memset(t, 0, sizeof(t));
  if (!title || title((uint32_t)id, t) != 0) t[0] = '\0';
  t[sizeof(t) - 1] = '\0';
  snprintf(title_id, n, "%s", t);
  *app_id = (uint32_t)id;
  return 0;
}

int plat_kill_app(uint32_t app_id, char *report, size_t n) {
  int rc = sceLncUtilKillApp(app_id);
  if (rc == 0) {
    snprintf(report, n, "kill %08x -> ok", app_id);
    return 0;
  }
  int (*sskill)(int, int, int, int) = sysfn("sceSystemServiceKillApp");
  int rc2 = sskill ? sskill((int)app_id, -1, 0, 0) : -1;
  snprintf(report, n, "kill %08x -> 0x%08X, system kill -> 0x%08X", app_id, (unsigned)rc,
           (unsigned)rc2);
  return rc2 == 0 ? 0 : -1;
}

int plat_boot_id(char *out, size_t n) {
  struct timeval tv;
  size_t len = sizeof(tv);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &tv, &len, NULL, 0) != 0 || tv.tv_sec <= 0) return -1;
  snprintf(out, n, "%lld", (long long)tv.tv_sec);
  return 0;
}

int plat_get_ip(char *buf, size_t n) {
  struct ifaddrs *ifaddr, *ifa;
  if (getifaddrs(&ifaddr) == -1) return -1;
  int ret = -1;
  for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;
    if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), buf, n, NULL, 0,
                    NI_NUMERICHOST) == 0) {
      ret = 0;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return ret;
}

int plat_reload_shellui(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) || !size) return -1;
  size += size / 4;
  char *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  pid_t target = -1;
  for (char *p = buf; p + sizeof(int) <= buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    if (ki->ki_structsize <= 0) break;
    if (strcmp(ki->ki_comm, "SceShellUI") == 0) target = ki->ki_pid;
    p += ki->ki_structsize;
  }
  free(buf);
  if (target <= 0) return -1;
  /* SceShellCore restarts the home screen automatically */
  return kill(target, SIGKILL) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* HTTPS download                                                            */
/* ------------------------------------------------------------------------- */

static int g_http_tmpl = -1;

static int http_setup(void) {
  if (g_http_tmpl >= 0) return 0;
  sceNetInit(); /* fine if the loader already did this */
  int pool = sceNetPoolCreate("homehider", 64 * 1024, 0);
  if (pool < 0) return pool;
  int ssl = sceSslInit(256 * 1024);
  if (ssl < 0) return ssl;
  int ctx = sceHttp2Init(pool, ssl, 256 * 1024, 2);
  if (ctx < 0) return ctx;
  int tmpl = sceHttp2CreateTemplate(ctx, "HomeHider-PS5", 3, 1);
  if (tmpl < 0) return tmpl;
  sceHttp2SetAutoRedirect(tmpl, 1);
  g_http_tmpl = tmpl;
  return 0;
}

/* Location header from a 3xx response (fallback if auto-redirect is off). */
static int redirect_target(int req, char *out, size_t outsz) {
  char *hdr = NULL;
  size_t hsz = 0;
  if (sceHttp2GetAllResponseHeaders(req, &hdr, &hsz) != 0 || !hdr) return -1;
  for (size_t i = 0; i + 9 < hsz; i++) {
    if ((i == 0 || hdr[i - 1] == '\n') && strncasecmp(hdr + i, "location:", 9) == 0) {
      const char *v = hdr + i + 9;
      while (*v == ' ') v++;
      size_t n = 0;
      while (v + n < hdr + hsz && v[n] != '\r' && v[n] != '\n') n++;
      if (n == 0 || n >= outsz) return -1;
      memcpy(out, v, n);
      out[n] = '\0';
      return 0;
    }
  }
  return -1;
}

int plat_http_get(const char *url, unsigned char **out, size_t *out_len,
                  size_t max, volatile long long *progress) {
  *out = NULL;
  *out_len = 0;
  int rc = http_setup();
  if (rc) return rc < 0 ? rc : -1;

  char cur[1024];
  snprintf(cur, sizeof(cur), "%s", url);
  for (int hop = 0; hop < 6; hop++) {
    int req = sceHttp2CreateRequestWithURL(g_http_tmpl, "GET", cur, 0);
    if (req < 0) return req;
    if ((rc = sceHttp2SendRequest(req, NULL, 0)) != 0) {
      sceHttp2DeleteRequest(req);
      return rc < 0 ? rc : -1;
    }
    int status = 0;
    if (sceHttp2GetStatusCode(req, &status) != 0) {
      sceHttp2DeleteRequest(req);
      return -1;
    }
    if (status >= 300 && status < 400) {
      int ok = redirect_target(req, cur, sizeof(cur)) == 0;
      sceHttp2DeleteRequest(req);
      if (!ok) return status;
      continue;
    }
    if (status != 200) {
      sceHttp2DeleteRequest(req);
      return status;
    }
    size_t cap = 64 * 1024, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
      sceHttp2DeleteRequest(req);
      return -1;
    }
    for (;;) {
      if (len == cap) {
        if (cap >= max) {
          free(buf);
          sceHttp2DeleteRequest(req);
          return -3;
        }
        size_t ncap = cap * 2 > max ? max : cap * 2;
        unsigned char *nb = realloc(buf, ncap);
        if (!nb) {
          free(buf);
          sceHttp2DeleteRequest(req);
          return -1;
        }
        buf = nb;
        cap = ncap;
      }
      int n = sceHttp2ReadData(req, buf + len, cap - len);
      if (n < 0) {
        free(buf);
        sceHttp2DeleteRequest(req);
        return n;
      }
      if (n == 0) break;
      len += (size_t)n;
      if (progress) *progress = (long long)len;
    }
    sceHttp2DeleteRequest(req);
    *out = buf;
    *out_len = len;
    return 200;
  }
  return -4; /* too many redirects */
}

/* ------------------------------------------------------------------------- */
/* tiles                                                                     */
/* ------------------------------------------------------------------------- */

int plat_app_install(const char *title_id, const char *app_dir) {
  int err = sceAppInstUtilInitialize();
  if (err) return err;
  /* resolved at run time, like Payload Manager, so a missing export on some
   * firmware can't stop the whole payload from loading */
  int (*install_dir)(const char *, const char *, void *) = NULL;
  uint32_t handle;
  if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
    install_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
  char dir[300];
  snprintf(dir, sizeof(dir), "%s/", app_dir);
  err = install_dir ? install_dir(title_id, dir, 0) : sceAppInstUtilAppInstallAll(0);
  sceAppInstUtilTerminate();
  return err;
}

int plat_app_uninstall(const char *title_id) {
  int err = sceAppInstUtilInitialize();
  if (err) return err;
  err = sceAppInstUtilAppUnInstall(title_id);
  sceAppInstUtilTerminate();
  return err;
}

