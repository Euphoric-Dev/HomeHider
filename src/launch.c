#include "launch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "log.h"
#include "platform.h"

static struct {
  char app_dir[256];
  char appmeta_dir[256];
} C = {"/user/app", "/user/appmeta"};

void launch_init(const launch_cfg_t *cfg) {
  snprintf(C.app_dir, sizeof(C.app_dir), "%s", cfg->app_dir);
  snprintf(C.appmeta_dir, sizeof(C.appmeta_dir), "%s", cfg->appmeta_dir);
}

/* ------------------------------------------------------------------------- */
/* web link tiles                                                            */
/* ------------------------------------------------------------------------- */

static int good_url(const char *u) {
  if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return 0;
  size_t n = strlen(u);
  if (n < 10 || n > 480) return 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)u[i];
    if (c <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' || c >= 0x7f) return 0;
  }
  return 1;
}

/* deeplinkUri from a JSON document, if it is a usable web address. */
static int url_from_json(const char *js, size_t len, char *url, size_t n) {
  int max = 4096;
  json_tok_t *t = malloc(sizeof(json_tok_t) * (size_t)max);
  if (!t) return -1;
  int nt = json_parse(js, len, t, max);
  int rc = -1;
  if (nt > 0 && t[0].type == JSON_OBJ) {
    char u[512];
    int i = json_get(js, t, nt, 0, "deeplinkUri");
    if (i >= 0 && json_str(js, t, i, u, sizeof(u)) == 0 && good_url(u)) {
      snprintf(url, n, "%s", u);
      rc = 0;
    }
  }
  free(t);
  return rc;
}

static int url_from_file(const char *path, char *url, size_t n) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  char *buf = malloc(128 * 1024);
  size_t len = buf ? fread(buf, 1, 128 * 1024 - 1, f) : 0;
  fclose(f);
  if (!buf) return -1;
  int rc = len > 0 ? url_from_json(buf, len, url, n) : -1;
  free(buf);
  return rc;
}

int launch_web_url(hh_t *h, const char *tid, char *url, size_t n) {
  char path[600];
  struct stat st;
  /* an app with a program of its own is started normally */
  snprintf(path, sizeof(path), "%s/%s/eboot.bin", C.app_dir, tid);
  if (stat(path, &st) == 0) return -1;

  snprintf(path, sizeof(path), "%s/%s/sce_sys/param.json", C.app_dir, tid);
  if (url_from_file(path, url, n) == 0) return 0;
  snprintf(path, sizeof(path), "%s/%s/param.json", C.appmeta_dir, tid);
  if (url_from_file(path, url, n) == 0) return 0;

  /* the console's own copy of the app's details */
  if (!h || !h->db) return -1;
  sqlite3_stmt *st2 = NULL;
  if (sqlite3_prepare_v2(h->db,
                         "SELECT AppInfoJson FROM main.tbl_contentinfo WHERE titleId=?1",
                         -1, &st2, NULL) != SQLITE_OK) {
    if (st2) sqlite3_finalize(st2);
    return -1; /* no such column on this firmware */
  }
  sqlite3_bind_text(st2, 1, tid, -1, SQLITE_TRANSIENT);
  int rc = -1;
  if (sqlite3_step(st2) == SQLITE_ROW && sqlite3_column_text(st2, 0)) {
    const char *js = (const char *)sqlite3_column_text(st2, 0);
    rc = url_from_json(js, (size_t)sqlite3_column_bytes(st2, 0), url, n);
  }
  sqlite3_finalize(st2);
  return rc;
}

/* ------------------------------------------------------------------------- */
/* launcher errors                                                           */
/* ------------------------------------------------------------------------- */

const char *launch_error_text(int rc) {
  switch ((unsigned)rc) {
    case LNC_ERR_APP_NOT_FOUND: return "The console can't find this app. It may have been uninstalled.";
    case 0x80940008:
    case 0x80940009: return "This app's files are missing or incomplete, so the console can't start it.";
    case 0x8094000a: return "No one is signed in on the PS5.";
    case 0x8094000d: return "The console can't start this right now. Close other apps and try again.";
    case 0x8094000f: return "The console didn't allow this app to start.";
    case 0x80940013: return "Insert the game disc first.";
    case 0x8094001c: return "The console doesn't recognise this title ID.";
    case LNC_ERR_NOT_INITIALIZED: return "The console's app launcher wasn't ready. Try again.";
    case 0x8094001a: return "A user is signing out. Try again in a moment.";
    default: return "The console couldn't start it.";
  }
}

/* ------------------------------------------------------------------------- */
/* jobs                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
  int  job;
  char tid[16];
  char name[160];
  int  close_running;
} job_args_t;

static struct {
  pthread_mutex_t mx;
  int  seq;
  int  busy;
  /* last job */
  int  job;
  char tid[16];
  char name[160];
  int  done;
  char result[16];   /* ok | error | other_running */
  char note[200];
  char error[300];
  unsigned code;
  char other_tid[16];
} J = {.mx = PTHREAD_MUTEX_INITIALIZER};

/* Closes the running game (after the user said OK) and waits until the
 * console reports it gone. */
static void close_running_game(const char *for_tid, uint32_t id, const char *otid) {
  char r[200];
  int rc = plat_kill_app(id, r, sizeof(r));
  hh_log("launch %s: closing %s first: %s", for_tid, otid[0] ? otid : "the running game", r);
  if (rc) return;
  for (int i = 0; i < 50; i++) {  /* up to 10 s */
    char t[64];
    uint32_t now;
    if (plat_running_game(t, sizeof(t), &now) || now != id) break;
    usleep(200000);
  }
  usleep(800000); /* let the console settle before the next start */
}

static void finish(const char *result, const char *note, const char *error, unsigned code,
                   const char *other) {
  pthread_mutex_lock(&J.mx);
  J.done = 1;
  snprintf(J.result, sizeof(J.result), "%s", result);
  snprintf(J.note, sizeof(J.note), "%s", note ? note : "");
  snprintf(J.error, sizeof(J.error), "%s", error ? error : "");
  J.code = code;
  snprintf(J.other_tid, sizeof(J.other_tid), "%s", other ? other : "");
  J.busy = 0;
  pthread_mutex_unlock(&J.mx);
}

static void *worker(void *arg) {
  job_args_t a = *(job_args_t *)arg;
  free(arg);
  char otid[64] = "";
  uint32_t oid = 0;

  if (a.close_running && plat_running_game(otid, sizeof(otid), &oid) == 0 &&
      strcmp(otid, a.tid) != 0)
    close_running_game(a.tid, oid, otid);

  uint32_t app_id = 0;
  int rc = plat_launch_app(a.tid, &app_id);
  hh_log("launch %s -> 0x%08X (app %08x)", a.tid, (unsigned)rc, app_id);

  char msg[300];
  if (rc == 0 || (unsigned)rc == LNC_ERR_ALREADY_RUNNING) {
    snprintf(msg, sizeof(msg), "%s %s", rc ? "Switching to" : "Starting", a.name);
    finish("ok", msg, NULL, (unsigned)rc, NULL);
    return NULL;
  }
  if ((unsigned)rc == LNC_ERR_KILL_NEEDED || (unsigned)rc == LNC_ERR_SUSPEND_NEEDED) {
    otid[0] = '\0';
    if (plat_running_game(otid, sizeof(otid), &oid) == 0 && otid[0] && strlen(otid) < 16) {
      finish("other_running", NULL, "Another game is running.", (unsigned)rc, otid);
      return NULL;
    }
    snprintf(msg, sizeof(msg), "Another game is running. Close it from the home screen, then try again "
             "(error 0x%08X).", (unsigned)rc);
    finish("error", NULL, msg, (unsigned)rc, NULL);
    return NULL;
  }
  snprintf(msg, sizeof(msg), "%s (error 0x%08X)", launch_error_text(rc), (unsigned)rc);
  finish("error", NULL, msg, (unsigned)rc, NULL);
  return NULL;
}

int launch_start(const char *tid, const char *name, int close_running) {
  job_args_t *a = calloc(1, sizeof(*a));
  if (!a) return -1;
  snprintf(a->tid, sizeof(a->tid), "%s", tid);
  snprintf(a->name, sizeof(a->name), "%s", name && name[0] ? name : tid);
  a->close_running = close_running;

  pthread_mutex_lock(&J.mx);
  if (J.busy) {
    pthread_mutex_unlock(&J.mx);
    free(a);
    return -1;
  }
  J.busy = 1;
  J.job = ++J.seq;
  a->job = J.job;
  snprintf(J.tid, sizeof(J.tid), "%s", a->tid);
  snprintf(J.name, sizeof(J.name), "%s", a->name);
  J.done = 0;
  J.result[0] = J.note[0] = J.error[0] = J.other_tid[0] = '\0';
  J.code = 0;
  int job = J.job;
  pthread_mutex_unlock(&J.mx);

  pthread_t t;
  if (pthread_create(&t, NULL, worker, a) != 0) {
    free(a);
    finish("error", NULL, "Couldn't start a worker thread.", 0, NULL);
    return job;
  }
  pthread_detach(t);
  return job;
}

void launch_status_json(int job, strbuf_t *sb,
                        void (*name_of)(const char *tid, char *out, size_t n)) {
  pthread_mutex_lock(&J.mx);
  if (job != J.job || job <= 0) {
    pthread_mutex_unlock(&J.mx);
    sb_puts(sb, "{\"ok\":false,\"error\":\"That launch is no longer tracked.\"}");
    return;
  }
  char tid[16], name[160], result[16], note[200], error[300], other[16];
  int done = J.done;
  unsigned code = J.code;
  memcpy(tid, J.tid, sizeof(tid));
  memcpy(name, J.name, sizeof(name));
  memcpy(result, J.result, sizeof(result));
  memcpy(note, J.note, sizeof(note));
  memcpy(error, J.error, sizeof(error));
  memcpy(other, J.other_tid, sizeof(other));
  pthread_mutex_unlock(&J.mx);

  sb_printf(sb, "{\"ok\":true,\"job\":%d,\"id\":\"%s\",\"name\":", job, tid);
  sb_json_str(sb, name);
  sb_printf(sb, ",\"done\":%s", done ? "true" : "false");
  if (done) {
    sb_puts(sb, ",\"result\":");
    sb_json_str(sb, result);
    sb_printf(sb, ",\"code\":\"0x%08X\",\"note\":", code);
    sb_json_str(sb, note[0] ? note : NULL);
    sb_puts(sb, ",\"error\":");
    sb_json_str(sb, error[0] ? error : NULL);
    if (other[0]) {
      char oname[160] = "";
      if (name_of) name_of(other, oname, sizeof(oname));
      sb_printf(sb, ",\"other\":{\"id\":\"%s\",\"name\":", other);
      sb_json_str(sb, oname[0] ? oname : other);
      sb_puts(sb, "}");
    }
  }
  sb_puts(sb, "}");
}
