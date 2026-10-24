#include "hider.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Tiles offered in the menu. Title IDs from psdevwiki (PS5 Title_ID page). */
const hh_target_t HH_TARGETS[] = {
    {"NPXS40047", "PlayStation Store", "Games"},
    {"NPXS40075", "Media Gallery", "Games"},
    {"NPXS40071", "Game Library", "Games"},
    {"NPXS40139", "App Library", "Media"},
    {"NPXS40056", "All Apps", "Media"},
};
const int HH_TARGET_COUNT = (int)(sizeof(HH_TARGETS) / sizeof(HH_TARGETS[0]));

/* Tiles the home screen adds by itself whatever the database says (seen on
 * FW 4.51: hiding them in app.db has no effect), so they are not offered.
 * "All Apps" (NPXS40056) does hide normally and is NOT in this list. */
static const char *const kLocked[] = {"NPXS40071", "NPXS40139"};

int hh_is_locked(const char *tid) {
  for (size_t i = 0; i < sizeof(kLocked) / sizeof(kLocked[0]); i++)
    if (strcmp(tid, kLocked[i]) == 0) return 1;
  return 0;
}

#define BUSY_TIMEOUT_MS 10000

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void set_err(hh_t *h, const char *fmt, const char *detail) {
  snprintf(h->err, sizeof(h->err), fmt, detail ? detail : "");
}

int hh_valid_tid(const char *tid) {
  if (!tid || strlen(tid) != 9) return 0;
  for (int i = 0; i < 4; i++)
    if (!isupper((unsigned char)tid[i])) return 0;
  for (int i = 4; i < 9; i++)
    if (!isdigit((unsigned char)tid[i])) return 0;
  return 1;
}

static int valid_ident_suffix(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n >= HH_USER_LEN) return 0;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
  return 1;
}

static int exec(hh_t *h, const char *sql) {
  char *msg = NULL;
  if (sqlite3_exec(h->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
    set_err(h, "Database error: %s", msg ? msg : sqlite3_errmsg(h->db));
    sqlite3_free(msg);
    return -1;
  }
  return 0;
}

/* Prepares sql and binds up to three text args (NULL = not bound). */
static sqlite3_stmt *prep(hh_t *h, const char *sql, const char *a1,
                          const char *a2, const char *a3) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    set_err(h, "Database error: %s", sqlite3_errmsg(h->db));
    return NULL;
  }
  if (a1) sqlite3_bind_text(st, 1, a1, -1, SQLITE_TRANSIENT);
  if (a2) sqlite3_bind_text(st, 2, a2, -1, SQLITE_TRANSIENT);
  if (a3) sqlite3_bind_text(st, 3, a3, -1, SQLITE_TRANSIENT);
  return st;
}

/* Runs a statement that returns no rows. */
static int run(hh_t *h, const char *sql, const char *a1, const char *a2,
               const char *a3) {
  sqlite3_stmt *st = prep(h, sql, a1, a2, a3);
  if (!st) return -1;
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    set_err(h, "Database error: %s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

/* Returns the first column of the first row as an integer, or dflt. */
static long long scalar(hh_t *h, const char *sql, const char *a1,
                        const char *a2, long long dflt) {
  sqlite3_stmt *st = prep(h, sql, a1, a2, NULL);
  if (!st) return dflt;
  long long v = dflt;
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static int has_table(hh_t *h, const char *schema, const char *table) {
  char sql[160];
  snprintf(sql, sizeof(sql),
           "SELECT count(*) FROM %s.sqlite_master WHERE type='table' AND name=?1",
           schema);
  return scalar(h, sql, table, NULL, 0) > 0;
}

static int has_col(hh_t *h, const char *schema, const char *table, const char *col) {
  sqlite3_stmt *st = prep(h,
      "SELECT count(*) FROM pragma_table_info(?1, ?2) WHERE name=?3",
      table, schema, col);
  long long n = 0;
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  return n > 0;
}

/* Builds a quoted, comma separated list of columns that exist in both
 * main.tbl_group_<u> and hh.grp_<u>, skipping an INTEGER PRIMARY KEY column
 * of the console table (a rowid alias the console assigns itself). */
static int group_cols(hh_t *h, const char *user, strbuf_t *out) {
  char main_t[96], bak_t[96];
  snprintf(main_t, sizeof(main_t), "tbl_group_%s", user);
  snprintf(bak_t, sizeof(bak_t), "grp_%s", user);
  sqlite3_stmt *st = prep(h,
      "SELECT m.name FROM pragma_table_info(?1, 'main') m "
      "JOIN pragma_table_info(?2, 'hh') b ON b.name = m.name "
      "WHERE NOT (m.pk = 1 AND upper(m.type) = 'INTEGER') ORDER BY m.cid",
      main_t, bak_t, NULL);
  if (!st) return -1;
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(st, 0);
    if (!c || strchr(c, '"')) continue;
    sb_printf(out, "%s\"%s\"", n ? "," : "", c);
    n++;
  }
  sqlite3_finalize(st);
  if (n == 0) {
    set_err(h, "Could not match MyHome backup columns%s", "");
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* open / close                                                              */
/* ------------------------------------------------------------------------- */

static void discover_users(hh_t *h) {
  h->nusers = 0;
  sqlite3_stmt *st = prep(h,
      "SELECT name FROM main.sqlite_master WHERE type='table' "
      "AND name LIKE 'tbl\\_iconinfo\\_%' ESCAPE '\\' ORDER BY name",
      NULL, NULL, NULL);
  if (!st) return;
  while (sqlite3_step(st) == SQLITE_ROW && h->nusers < HH_MAX_USERS) {
    const char *name = (const char *)sqlite3_column_text(st, 0);
    if (!name) continue;
    const char *u = name + strlen("tbl_iconinfo_");
    if (!valid_ident_suffix(u)) continue;
    snprintf(h->users[h->nusers], HH_USER_LEN, "%s", u);
    h->nusers++;
  }
  sqlite3_finalize(st);

  /* keep only users whose icon table has the columns we rely on */
  int kept = 0;
  for (int i = 0; i < h->nusers; i++) {
    char t[96], g[96];
    snprintf(t, sizeof(t), "tbl_iconinfo_%s", h->users[i]);
    if (!has_col(h, "main", t, "titleId") || !has_col(h, "main", t, "visible"))
      continue;
    if (kept != i) memcpy(h->users[kept], h->users[i], HH_USER_LEN);
    h->has_disp[kept] = has_col(h, "main", t, "dispLocation");
    snprintf(g, sizeof(g), "tbl_group_%s", h->users[kept]);
    h->has_group[kept] = has_table(h, "main", g) &&
                         has_col(h, "main", g, "itemId") &&
                         has_col(h, "main", g, "groupName");
    kept++;
  }
  h->nusers = kept;
}

int hh_open(hh_t *h, const char *app_db, const char *state_db) {
  memset(h, 0, sizeof(*h));
  snprintf(h->app_db_path, sizeof(h->app_db_path), "%s", app_db);

  struct stat sbuf;
  if (stat(app_db, &sbuf) != 0) {
    set_err(h, "Home screen database not found: %s", app_db);
    return -1;
  }
  /* never create app.db: READWRITE without CREATE */
  if (sqlite3_open_v2(app_db, &h->db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    set_err(h, "Cannot open app.db: %s", h->db ? sqlite3_errmsg(h->db) : "?");
    if (h->db) sqlite3_close(h->db);
    h->db = NULL;
    return -1;
  }
  sqlite3_busy_timeout(h->db, BUSY_TIMEOUT_MS);
  sqlite3_exec(h->db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

  /* ATTACH inherits app.db's no-create flag, so create state.db first */
  sqlite3 *sdb = NULL;
  char *msg = NULL;
  if (sqlite3_open_v2(state_db, &sdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK ||
      sqlite3_exec(sdb,
                   "CREATE TABLE IF NOT EXISTS hidden("
                   " title_id TEXT NOT NULL, user TEXT NOT NULL,"
                   " orig_visible INTEGER, orig_disp INTEGER, hidden_at TEXT,"
                   " PRIMARY KEY(title_id, user));"
                   "CREATE TABLE IF NOT EXISTS wanted("
                   " title_id TEXT PRIMARY KEY, hidden_at TEXT);"
                   "CREATE TABLE IF NOT EXISTS folders("
                   " title_id TEXT PRIMARY KEY, name TEXT NOT NULL,"
                   " tab TEXT NOT NULL DEFAULT 'games', color TEXT NOT NULL DEFAULT 'blue',"
                   " custom_icon INTEGER NOT NULL DEFAULT 0, created_at TEXT);"
                   "CREATE TABLE IF NOT EXISTS folder_items("
                   " title_id TEXT PRIMARY KEY, folder TEXT NOT NULL,"
                   " pos INTEGER NOT NULL DEFAULT 0, was_hidden INTEGER NOT NULL DEFAULT 0);"
                   "CREATE TABLE IF NOT EXISTS custom("
                   " title_id TEXT PRIMARY KEY, name TEXT, orig_name TEXT,"
                   " icon_file TEXT, icon_meta TEXT, updated_at TEXT, bg_file TEXT);"
                   "CREATE TABLE IF NOT EXISTS custom_cells("
                   " title_id TEXT NOT NULL, kind TEXT NOT NULL, tbl TEXT NOT NULL, col TEXT NOT NULL,"
                   " keycol TEXT NOT NULL, keyval TEXT NOT NULL, orig TEXT,"
                   " PRIMARY KEY(title_id, kind, tbl, col, keycol, keyval));",
                   NULL, NULL, &msg) != SQLITE_OK) {
    set_err(h, "Cannot create state.db: %s",
            msg ? msg : (sdb ? sqlite3_errmsg(sdb) : state_db));
    sqlite3_free(msg);
    if (sdb) sqlite3_close(sdb);
    hh_close(h);
    return -1;
  }
  /* columns added in later versions (fails harmlessly when present) */
  sqlite3_exec(sdb, "ALTER TABLE custom ADD COLUMN bg_file TEXT", NULL, NULL, NULL);
  sqlite3_close(sdb);

  sqlite3_stmt *st = prep(h, "ATTACH DATABASE ?1 AS hh", state_db, NULL, NULL);
  if (!st || sqlite3_step(st) != SQLITE_DONE) {
    set_err(h, "Cannot open state.db: %s", sqlite3_errmsg(h->db));
    if (st) sqlite3_finalize(st);
    hh_close(h);
    return -1;
  }
  sqlite3_finalize(st);

  discover_users(h);
  return 0;
}

void hh_close(hh_t *h) {
  if (h->db) sqlite3_close(h->db);
  h->db = NULL;
}

static int need_users(hh_t *h) {
  if (h->nusers > 0) return 0;
  set_err(h, "This firmware's home screen database has a layout Home Hider "
             "doesn't know yet (no usable tbl_iconinfo_* tables). Open "
             "Advanced > Diagnostics and share the output.%s", "");
  return -1;
}

/* ------------------------------------------------------------------------- */
/* hide / unhide                                                             */
/* ------------------------------------------------------------------------- */

/* Hides tid for one user. *found is set when the user has the title. */
static int hide_for_user(hh_t *h, int ui, const char *tid, int *found) {
  const char *u = h->users[ui];
  char sql[512];

  /* 1+2: icon row -> remember original, then visible=0 and clear tab bits */
  snprintf(sql, sizeof(sql),
           "SELECT visible, %s FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
           h->has_disp[ui] ? "dispLocation" : "NULL", u);
  sqlite3_stmt *st = prep(h, sql, tid, NULL, NULL);
  if (!st) return -1;
  int have_row = 0;
  long long vis = 1, disp = 0;
  int disp_null = 1;
  if (sqlite3_step(st) == SQLITE_ROW) {
    have_row = 1;
    vis = sqlite3_column_int64(st, 0);
    disp_null = sqlite3_column_type(st, 1) == SQLITE_NULL;
    disp = sqlite3_column_int64(st, 1);
  }
  sqlite3_finalize(st);

  if (have_row) {
    *found = 1;
    /* INSERT OR IGNORE keeps the very first original if we hide again */
    st = prep(h,
              "INSERT OR IGNORE INTO hh.hidden(title_id, user, orig_visible, "
              "orig_disp, hidden_at) VALUES(?1, ?2, ?3, ?4, datetime('now'))",
              tid, u, NULL);
    if (!st) return -1;
    sqlite3_bind_int64(st, 3, vis);
    if (disp_null) sqlite3_bind_null(st, 4);
    else sqlite3_bind_int64(st, 4, disp);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      set_err(h, "Database error: %s", sqlite3_errmsg(h->db));
      return -1;
    }

    if (h->has_disp[ui])
      snprintf(sql, sizeof(sql),
               "UPDATE main.\"tbl_iconinfo_%s\" SET visible=0, "
               "dispLocation=(dispLocation & ~%d) WHERE titleId=?1",
               u, HH_BIT_GAMES | HH_BIT_MEDIA);
    else
      snprintf(sql, sizeof(sql),
               "UPDATE main.\"tbl_iconinfo_%s\" SET visible=0 WHERE titleId=?1", u);
    if (run(h, sql, tid, NULL, NULL)) return -1;
  }

  /* 3: MyHome pin row -> back up (once), then remove */
  if (h->has_group[ui]) {
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM main.\"tbl_group_%s\" "
             "WHERE itemId=?1 AND groupName='MyHome'", u);
    if (scalar(h, sql, tid, NULL, 0) > 0) {
      *found = 1;
      snprintf(sql, sizeof(sql),
               "CREATE TABLE IF NOT EXISTS hh.\"grp_%s\" AS "
               "SELECT * FROM main.\"tbl_group_%s\" WHERE 0", u, u);
      if (exec(h, sql)) return -1;

      snprintf(sql, sizeof(sql),
               "SELECT count(*) FROM hh.\"grp_%s\" "
               "WHERE itemId=?1 AND groupName='MyHome'", u);
      if (scalar(h, sql, tid, NULL, 0) == 0) {
        strbuf_t cols;
        sb_init(&cols);
        if (group_cols(h, u, &cols)) {
          sb_free(&cols);
          return -1;
        }
        strbuf_t q;
        sb_init(&q);
        sb_printf(&q,
                  "INSERT INTO hh.\"grp_%s\"(%s) SELECT %s FROM main.\"tbl_group_%s\" "
                  "WHERE itemId=?1 AND groupName='MyHome'",
                  u, cols.data, cols.data, u);
        int rc = q.oom ? -1 : run(h, q.data, tid, NULL, NULL);
        sb_free(&q);
        sb_free(&cols);
        if (rc) return -1;
      }

      snprintf(sql, sizeof(sql),
               "DELETE FROM main.\"tbl_group_%s\" "
               "WHERE itemId=?1 AND groupName='MyHome'", u);
      if (run(h, sql, tid, NULL, NULL)) return -1;
    }
  }
  return 0;
}

int hh_hide(hh_t *h, const char *tid) {
  if (!hh_valid_tid(tid)) {
    set_err(h, "Invalid title id: %s", tid);
    return -1;
  }
  if (hh_is_locked(tid)) {
    set_err(h, "The home screen always adds this tile (%s) by itself, so it "
               "can't be hidden from the database.", tid);
    return -1;
  }
  if (strncmp(tid, "HHFD", 4) == 0) {
    set_err(h, "%s is a Home Hider folder. Manage it in the Folders section.", tid);
    return -1;
  }
  if (need_users(h)) return -1;
  if (exec(h, "BEGIN IMMEDIATE")) return -1;

  int found = 0;
  for (int i = 0; i < h->nusers; i++) {
    if (hide_for_user(h, i, tid, &found)) {
      exec(h, "ROLLBACK");
      return -1;
    }
  }
  if (!found) {
    exec(h, "ROLLBACK");
    set_err(h, "%s is not on this console's home screen database.", tid);
    return -1;
  }
  if (run(h, "INSERT OR REPLACE INTO hh.wanted(title_id, hidden_at) "
             "VALUES(?1, datetime('now'))", tid, NULL, NULL)) {
    exec(h, "ROLLBACK");
    return -1;
  }
  if (exec(h, "COMMIT")) {
    exec(h, "ROLLBACK");
    return -1;
  }
  return 0;
}

static int unhide_for_user(hh_t *h, int ui, const char *tid) {
  const char *u = h->users[ui];
  char sql[512];

  sqlite3_stmt *st = prep(h,
      "SELECT orig_visible, orig_disp FROM hh.hidden WHERE title_id=?1 AND user=?2",
      tid, u, NULL);
  if (!st) return -1;
  int saved = 0, disp_null = 1;
  long long ovis = 1, odisp = 0;
  if (sqlite3_step(st) == SQLITE_ROW) {
    saved = 1;
    if (sqlite3_column_type(st, 0) != SQLITE_NULL) ovis = sqlite3_column_int64(st, 0);
    disp_null = sqlite3_column_type(st, 1) == SQLITE_NULL;
    odisp = sqlite3_column_int64(st, 1);
  }
  sqlite3_finalize(st);

  char vis_s[24], disp_s[24];
  snprintf(vis_s, sizeof(vis_s), "%lld", saved ? (ovis ? ovis : 1) : 1);
  snprintf(disp_s, sizeof(disp_s), "%lld", odisp);
  if (saved && h->has_disp[ui] && !disp_null)
    snprintf(sql, sizeof(sql),
             "UPDATE main.\"tbl_iconinfo_%s\" SET visible=CAST(?2 AS INTEGER), "
             "dispLocation=CAST(?3 AS INTEGER) WHERE titleId=?1", u);
  else
    snprintf(sql, sizeof(sql),
             "UPDATE main.\"tbl_iconinfo_%s\" SET visible=CAST(?2 AS INTEGER) "
             "WHERE titleId=?1", u);
  if (run(h, sql, tid, vis_s, disp_s)) return -1;

  char bak[96];
  snprintf(bak, sizeof(bak), "grp_%s", u);
  if (h->has_group[ui] && has_table(h, "hh", bak)) {
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM hh.\"grp_%s\" WHERE itemId=?1 AND groupName='MyHome'", u);
    if (scalar(h, sql, tid, NULL, 0) > 0) {
      strbuf_t cols;
      sb_init(&cols);
      if (group_cols(h, u, &cols)) {
        sb_free(&cols);
        return -1;
      }
      /* avoid duplicates if the system already re-pinned it */
      snprintf(sql, sizeof(sql),
               "DELETE FROM main.\"tbl_group_%s\" WHERE itemId=?1 AND groupName='MyHome'", u);
      int rc = run(h, sql, tid, NULL, NULL);
      if (!rc) {
        strbuf_t q;
        sb_init(&q);
        sb_printf(&q,
                  "INSERT INTO main.\"tbl_group_%s\"(%s) SELECT %s FROM hh.\"grp_%s\" "
                  "WHERE itemId=?1 AND groupName='MyHome'",
                  u, cols.data, cols.data, u);
        rc = q.oom ? -1 : run(h, q.data, tid, NULL, NULL);
        sb_free(&q);
      }
      sb_free(&cols);
      if (rc) return -1;
      snprintf(sql, sizeof(sql),
               "DELETE FROM hh.\"grp_%s\" WHERE itemId=?1 AND groupName='MyHome'", u);
      if (run(h, sql, tid, NULL, NULL)) return -1;
    }
  }

  return run(h, "DELETE FROM hh.hidden WHERE title_id=?1 AND user=?2", tid, u, NULL);
}

int hh_unhide(hh_t *h, const char *tid) {
  if (!hh_valid_tid(tid)) {
    set_err(h, "Invalid title id: %s", tid);
    return -1;
  }
  if (need_users(h)) return -1;
  if (exec(h, "BEGIN IMMEDIATE")) return -1;
  for (int i = 0; i < h->nusers; i++) {
    if (unhide_for_user(h, i, tid)) {
      exec(h, "ROLLBACK");
      return -1;
    }
  }
  if (run(h, "DELETE FROM hh.wanted WHERE title_id=?1", tid, NULL, NULL) ||
      exec(h, "COMMIT")) {
    exec(h, "ROLLBACK");
    return -1;
  }
  return 0;
}

/* Collects title ids from hh.wanted (max n). */
static int wanted_list(hh_t *h, char ids[][16], int n) {
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.wanted ORDER BY title_id",
                          NULL, NULL, NULL);
  if (!st) return -1;
  int c = 0;
  while (c < n && sqlite3_step(st) == SQLITE_ROW) {
    const char *t = (const char *)sqlite3_column_text(st, 0);
    if (t && hh_valid_tid(t)) snprintf(ids[c++], 16, "%s", t);
  }
  sqlite3_finalize(st);
  return c;
}

#define WANTED_MAX 4096

int hh_unhide_all(hh_t *h, int *count) {
  char (*ids)[16] = malloc(WANTED_MAX * sizeof(*ids));
  if (!ids) return -1;
  int n = wanted_list(h, ids, WANTED_MAX), rc = n < 0 ? -1 : 0;
  *count = 0;
  for (int i = 0; i < n; i++) {
    /* games inside folders stay hidden; they're managed by the folder */
    if (scalar(h, "SELECT count(*) FROM hh.folder_items WHERE title_id=?1", ids[i], NULL, 0))
      continue;
    if (hh_unhide(h, ids[i])) {
      rc = -1;
      break;
    }
    (*count)++;
  }
  free(ids);
  return rc;
}

int hh_release_locked(hh_t *h, int *count) {
  *count = 0;
  if (h->nusers == 0) return 0;
  for (size_t i = 0; i < sizeof(kLocked) / sizeof(kLocked[0]); i++) {
    if (scalar(h, "SELECT count(*) FROM hh.wanted WHERE title_id=?1", kLocked[i],
               NULL, 0) == 0)
      continue;
    if (hh_unhide(h, kLocked[i])) return -1;
    (*count)++;
  }
  return 0;
}

int hh_reapply(hh_t *h, int *fixed) {
  *fixed = 0;
  if (h->nusers == 0) return 0;
  char (*ids)[16] = malloc(WANTED_MAX * sizeof(*ids));
  if (!ids) return -1;
  int n = wanted_list(h, ids, WANTED_MAX), rc = n < 0 ? -1 : 0;
  for (int i = 0; i < n; i++) {
    hh_state_t s;
    if (hh_is_locked(ids[i])) continue;
    if (hh_get_state(h, ids[i], &s)) { rc = -1; break; }
    if (!s.present || (!s.visible && !s.in_myhome)) continue;
    if (hh_hide(h, ids[i])) { rc = -1; break; }
    (*fixed)++;
  }
  free(ids);
  return rc;
}

int hh_get_state(hh_t *h, const char *tid, hh_state_t *s) {
  memset(s, 0, sizeof(*s));
  s->disp = -1;
  if (!hh_valid_tid(tid)) {
    set_err(h, "Invalid title id: %s", tid);
    return -1;
  }
  char sql[256];
  for (int i = 0; i < h->nusers; i++) {
    const char *u = h->users[i];
    snprintf(sql, sizeof(sql),
             "SELECT visible, %s FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
             h->has_disp[i] ? "dispLocation" : "-1", u);
    sqlite3_stmt *st = prep(h, sql, tid, NULL, NULL);
    if (!st) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
      s->present = 1;
      if (sqlite3_column_int64(st, 0) != 0) s->visible = 1;
      if (s->disp < 0) s->disp = (int)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    if (h->has_group[i]) {
      snprintf(sql, sizeof(sql),
               "SELECT count(*) FROM main.\"tbl_group_%s\" "
               "WHERE itemId=?1 AND groupName='MyHome'", u);
      if (scalar(h, sql, tid, NULL, 0) > 0) {
        s->present = 1;
        s->in_myhome = 1;
      }
    }
  }
  s->wanted = scalar(h, "SELECT count(*) FROM hh.wanted WHERE title_id=?1",
                     tid, NULL, 0) > 0;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* full listing                                                              */
/* ------------------------------------------------------------------------- */

/* Disc placeholders and similar internal entries (never real tiles). */
static const char *const kInternal[] = {
    "NPXS40140", "NPXS40144", "NPXS40145", "NPXS40148",
    "NPXS40149", "NPXS40150", "NPXS40205", "NPXS40106",
};

int hh_is_internal(const char *tid) {
  for (size_t i = 0; i < sizeof(kInternal) / sizeof(kInternal[0]); i++)
    if (strcmp(tid, kInternal[i]) == 0) return 1;
  return 0;
}

#define LIST_MAX 2048

typedef struct {
  char      id[16];
  char      name[160];
  long long disp;        /* first user's dispLocation, -1 unknown */
  long long access;      /* max lastAccessIndex across users */
  int       vis_any;
  int       pinned;      /* in a MyHome group now */
  int       installed;   /* installStatus == 2 (or column missing) */
  int       on_list;     /* on Home Hider's hide list */
  char      folder[16];  /* folder holding this title, "" if none */
  int       renamed;     /* Home Hider changed its name */
  int       custom_icon; /* Home Hider changed its picture */
  int       custom_bg;   /* Home Hider changed its background */
  long long orig_disp;   /* dispLocation before we hid it, -1 unknown */
  int       orig_pinned; /* we removed a MyHome pin */
} list_item_t;

static list_item_t *find_item(list_item_t *items, int n, const char *id) {
  for (int i = 0; i < n; i++)
    if (strcmp(items[i].id, id) == 0) return &items[i];
  return NULL;
}

static const char *tab_of(long long disp, int pinned) {
  if (pinned) return "games";
  if (disp >= 0 && (disp & HH_BIT_GAMES)) return "games";
  if (disp >= 0 && (disp & HH_BIT_MEDIA)) return "media";
  return "none";
}

static int cmp_items(const void *a, const void *b) {
  const list_item_t *x = a, *y = b;
  if (x->access != y->access) return x->access < y->access ? 1 : -1;
  return strcmp(x->name, y->name);
}

int hh_list(hh_t *h, strbuf_t *out) {
  list_item_t *items = calloc(LIST_MAX, sizeof(list_item_t));
  if (!items) {
    set_err(h, "Out of memory%s", "");
    return -1;
  }
  int n = 0;
  char sql[768];

  for (int ui = 0; ui < h->nusers; ui++) {
    const char *u = h->users[ui];
    char icon_t[96], con_t[96];
    snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", u);
    snprintf(con_t, sizeof(con_t), "tbl_concepticoninfo_%s", u);
    int has_lcid = has_col(h, "main", icon_t, "localConceptId");
    int has_name = has_col(h, "main", icon_t, "titleName");
    int has_acc = has_col(h, "main", icon_t, "lastAccessIndex");
    int has_inst = has_col(h, "main", icon_t, "installStatus");
    int has_con = has_lcid && has_table(h, "main", con_t) &&
                  has_col(h, "main", con_t, "localConceptId") &&
                  has_col(h, "main", con_t, "conceptName");

    snprintf(sql, sizeof(sql),
             "SELECT i.titleId, %s, i.visible, %s, %s, %s "
             "FROM main.\"%s\" i",
             has_con ? (has_name ? "coalesce(nullif(c.conceptName,''), i.titleName)"
                                 : "c.conceptName")
                     : (has_name ? "i.titleName" : "NULL"),
             h->has_disp[ui] ? "i.dispLocation" : "-1",
             has_acc ? "i.lastAccessIndex" : "0",
             has_inst ? "i.installStatus" : "2",
             icon_t);
    if (has_con) {
      /* separate buffer so the join clause can name both tables */
      char join[256];
      snprintf(join, sizeof(join),
               " LEFT JOIN main.\"%s\" c ON c.localConceptId = i.localConceptId", con_t);
      strncat(sql, join, sizeof(sql) - strlen(sql) - 1);
    }

    sqlite3_stmt *st = prep(h, sql, NULL, NULL, NULL);
    if (!st) {
      free(items);
      return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      if (!id || !hh_valid_tid(id) || hh_is_internal(id) || !strncmp(id, "HHFD", 4)) continue;
      list_item_t *it = find_item(items, n, id);
      if (!it) {
        if (n >= LIST_MAX) continue;
        it = &items[n++];
        snprintf(it->id, sizeof(it->id), "%s", id);
        it->disp = -1;
        it->orig_disp = -1;
        it->access = LLONG_MIN;
      }
      const char *name = (const char *)sqlite3_column_text(st, 1);
      if (!it->name[0] && name && name[0]) snprintf(it->name, sizeof(it->name), "%s", name);
      if (sqlite3_column_int64(st, 2) != 0) it->vis_any = 1;
      if (it->disp < 0) it->disp = sqlite3_column_int64(st, 3);
      long long acc = sqlite3_column_int64(st, 4);
      if (acc > it->access) it->access = acc;
      if (sqlite3_column_int64(st, 5) == 2) it->installed = 1;
    }
    sqlite3_finalize(st);

    if (h->has_group[ui]) {
      snprintf(sql, sizeof(sql),
               "SELECT itemId FROM main.\"tbl_group_%s\" WHERE groupName='MyHome'", u);
      st = prep(h, sql, NULL, NULL, NULL);
      if (st) {
        while (sqlite3_step(st) == SQLITE_ROW) {
          const char *id = (const char *)sqlite3_column_text(st, 0);
          list_item_t *it = id ? find_item(items, n, id) : NULL;
          if (it) it->pinned = 1;
        }
        sqlite3_finalize(st);
      }
      char bak[96];
      snprintf(bak, sizeof(bak), "grp_%s", u);
      if (has_table(h, "hh", bak)) {
        snprintf(sql, sizeof(sql),
                 "SELECT itemId FROM hh.\"grp_%s\" WHERE groupName='MyHome'", u);
        st = prep(h, sql, NULL, NULL, NULL);
        if (st) {
          while (sqlite3_step(st) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(st, 0);
            list_item_t *it = id ? find_item(items, n, id) : NULL;
            if (it) it->orig_pinned = 1;
          }
          sqlite3_finalize(st);
        }
      }
    }
  }

  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.wanted", NULL, NULL, NULL);
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      list_item_t *it = id ? find_item(items, n, id) : NULL;
      if (it) it->on_list = 1;
    }
    sqlite3_finalize(st);
  }
  st = prep(h, "SELECT title_id, folder FROM hh.folder_items", NULL, NULL, NULL);
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      list_item_t *it = id ? find_item(items, n, id) : NULL;
      if (it) snprintf(it->folder, sizeof(it->folder), "%s", sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
  }
  st = prep(h, "SELECT title_id, name IS NOT NULL, icon_file IS NOT NULL, bg_file IS NOT NULL FROM hh.custom",
            NULL, NULL, NULL);
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      list_item_t *it = id ? find_item(items, n, id) : NULL;
      if (!it) continue;
      it->renamed = sqlite3_column_int(st, 1);
      it->custom_icon = sqlite3_column_int(st, 2);
      it->custom_bg = sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
  }
  st = prep(h, "SELECT title_id, orig_disp FROM hh.hidden WHERE orig_disp IS NOT NULL",
            NULL, NULL, NULL);
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      list_item_t *it = id ? find_item(items, n, id) : NULL;
      if (it && it->orig_disp < 0) it->orig_disp = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
  }

  /* fall back to tbl_contentinfo for missing names */
  if (has_table(h, "main", "tbl_contentinfo") &&
      has_col(h, "main", "tbl_contentinfo", "titleName")) {
    for (int i = 0; i < n; i++) {
      if (items[i].name[0]) continue;
      st = prep(h, "SELECT titleName FROM main.tbl_contentinfo WHERE titleId=?1",
                items[i].id, NULL, NULL);
      if (!st) continue;
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        snprintf(items[i].name, sizeof(items[i].name), "%s",
                 (const char *)sqlite3_column_text(st, 0));
      sqlite3_finalize(st);
    }
  }

  qsort(items, (size_t)n, sizeof(list_item_t), cmp_items);

  sb_puts(out, "[");
  int first = 1;
  for (int i = 0; i < n; i++) {
    list_item_t *it = &items[i];
    int system = strncmp(it->id, "NPXS", 4) == 0;
    int locked = hh_is_locked(it->id);
    int hidden = !locked && !it->vis_any && !it->pinned;
    int on_home = it->vis_any && (it->pinned || (it->disp >= 0 && (it->disp & 6)));
    /* games/apps: everything installed. system tiles: only real home tiles */
    if (locked) continue; /* not hideable, so not offered */
    if (system ? !(on_home || it->on_list) : !(it->installed || it->on_list)) continue;

    /* for tiles we hid, report the tab they came from */
    const char *tab = it->on_list && hidden
                          ? tab_of(it->orig_disp >= 0 ? it->orig_disp : it->disp,
                                   it->orig_pinned)
                          : tab_of(it->disp, it->pinned);
    sb_printf(out, "%s{\"id\":\"%s\",\"name\":", first ? "" : ",", it->id);
    sb_json_str(out, it->name[0] ? it->name : it->id);
    sb_printf(out,
              ",\"kind\":\"%s\",\"tab\":\"%s\",\"hidden\":%s,\"on_list\":%s,"
              "\"pinned\":%s,\"recent\":%lld,\"folder\":",
              system ? "system" : "game", tab, hidden ? "true" : "false",
              it->on_list ? "true" : "false",
              (it->pinned || (hidden && it->orig_pinned)) ? "true" : "false",
              it->access == LLONG_MIN ? 0 : it->access);
    sb_json_str(out, it->folder[0] ? it->folder : NULL);
    sb_printf(out, ",\"renamed\":%s,\"custom_icon\":%s,\"custom_bg\":%s}",
              it->renamed ? "true" : "false", it->custom_icon ? "true" : "false",
              it->custom_bg ? "true" : "false");
    first = 0;
  }
  sb_puts(out, "]");
  free(items);
  return out->oom ? -1 : 0;
}

void hh_title_name(hh_t *h, const char *tid, char *out, size_t n) {
  out[0] = '\0';
  char sql[512];
  for (int ui = 0; h->db && ui < h->nusers && !out[0]; ui++) {
    char icon_t[96], con_t[96];
    snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", h->users[ui]);
    snprintf(con_t, sizeof(con_t), "tbl_concepticoninfo_%s", h->users[ui]);
    int has_name = has_col(h, "main", icon_t, "titleName");
    int has_con = has_col(h, "main", icon_t, "localConceptId") && has_table(h, "main", con_t) &&
                  has_col(h, "main", con_t, "localConceptId") &&
                  has_col(h, "main", con_t, "conceptName");
    if (!has_con && !has_name) continue;
    if (has_con)
      snprintf(sql, sizeof(sql),
               "SELECT %s FROM main.\"%s\" i LEFT JOIN main.\"%s\" c "
               "ON c.localConceptId = i.localConceptId WHERE i.titleId=?1",
               has_name ? "coalesce(nullif(c.conceptName,''), i.titleName)" : "c.conceptName",
               icon_t, con_t);
    else
      snprintf(sql, sizeof(sql), "SELECT titleName FROM main.\"%s\" WHERE titleId=?1", icon_t);
    sqlite3_stmt *st = prep(h, sql, tid, NULL, NULL);
    if (!st) continue;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
      snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  if (!out[0] && h->db && has_table(h, "main", "tbl_contentinfo") &&
      has_col(h, "main", "tbl_contentinfo", "titleName")) {
    sqlite3_stmt *st = prep(h, "SELECT titleName FROM main.tbl_contentinfo WHERE titleId=?1",
                            tid, NULL, NULL);
    if (st) {
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
      sqlite3_finalize(st);
    }
  }
}

/* Accepts only local png/jpg files in folders where the console keeps
 * app artwork; strips any "?query" suffix. */
static int safe_icon_path(const char *in, char *out, size_t outsz) {
  if (!in || in[0] != '/') return 0;
  size_t n = strcspn(in, "?#");
  if (n == 0 || n >= outsz) return 0;
  memcpy(out, in, n);
  out[n] = '\0';
  if (strstr(out, "..") || strstr(out, "//")) return 0;
  static const char *const prefixes[] = {
      "/user/appmeta/", "/system_ex/", "/system/", "/system_data/priv/appmeta/",
      "/user/app/",
  };
  int ok = 0;
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    if (strncmp(out, prefixes[i], strlen(prefixes[i])) == 0) ok = 1;
  if (!ok) return 0;
  const char *dot = strrchr(out, '.');
  if (!dot) return 0;
  return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
         strcasecmp(dot, ".jpeg") == 0;
}

int hh_icon_path(hh_t *h, const char *tid, char *out, size_t outsz) {
  if (!hh_valid_tid(tid)) return -1;
  struct stat sbuf;
  char sql[512], cand[512];

  /* 0. a picture chosen in Home Hider */
  if (h->db) {
    sqlite3_stmt *st = prep(h, "SELECT icon_file FROM hh.custom WHERE title_id=?1 AND icon_file IS NOT NULL",
                            tid, NULL, NULL);
    if (st) {
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) &&
          stat((const char *)sqlite3_column_text(st, 0), &sbuf) == 0) {
        snprintf(out, outsz, "%s", (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        return 0;
      }
      sqlite3_finalize(st);
    }
  }
  /* 1. what the home screen itself uses (icon0Info) */
  for (int ui = 0; h->db && ui < h->nusers; ui++) {
    const char *u = h->users[ui];
    char icon_t[96], con_t[96];
    snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", u);
    snprintf(con_t, sizeof(con_t), "tbl_concepticoninfo_%s", u);
    if (has_table(h, "main", con_t) && has_col(h, "main", con_t, "icon0Info") &&
        has_col(h, "main", icon_t, "localConceptId")) {
      snprintf(sql, sizeof(sql),
               "SELECT c.icon0Info FROM main.\"%s\" i JOIN main.\"%s\" c "
               "ON c.localConceptId = i.localConceptId WHERE i.titleId=?1",
               icon_t, con_t);
      sqlite3_stmt *st = prep(h, sql, tid, NULL, NULL);
      if (st) {
        while (sqlite3_step(st) == SQLITE_ROW) {
          if (safe_icon_path((const char *)sqlite3_column_text(st, 0), cand,
                             sizeof(cand)) &&
              stat(cand, &sbuf) == 0) {
            sqlite3_finalize(st);
            snprintf(out, outsz, "%s", cand);
            return 0;
          }
        }
        sqlite3_finalize(st);
      }
    }
    if (has_col(h, "main", icon_t, "icon0Info")) {
      snprintf(sql, sizeof(sql),
               "SELECT icon0Info FROM main.\"%s\" WHERE titleId=?1", icon_t);
      sqlite3_stmt *st = prep(h, sql, tid, NULL, NULL);
      if (st) {
        if (sqlite3_step(st) == SQLITE_ROW &&
            safe_icon_path((const char *)sqlite3_column_text(st, 0), cand,
                           sizeof(cand)) &&
            stat(cand, &sbuf) == 0) {
          sqlite3_finalize(st);
          snprintf(out, outsz, "%s", cand);
          return 0;
        }
        sqlite3_finalize(st);
      }
    }
  }
  if (h->db && has_table(h, "main", "tbl_contentinfo") &&
      has_col(h, "main", "tbl_contentinfo", "icon0Info")) {
    sqlite3_stmt *st = prep(h, "SELECT icon0Info FROM main.tbl_contentinfo WHERE titleId=?1",
                            tid, NULL, NULL);
    if (st) {
      if (sqlite3_step(st) == SQLITE_ROW &&
          safe_icon_path((const char *)sqlite3_column_text(st, 0), cand, sizeof(cand)) &&
          stat(cand, &sbuf) == 0) {
        sqlite3_finalize(st);
        snprintf(out, outsz, "%s", cand);
        return 0;
      }
      sqlite3_finalize(st);
    }
  }

#ifdef HH_HOST
  /* tests: artwork from a plain folder */
  if (getenv("HH_ICON_DIR")) {
    snprintf(cand, sizeof(cand), "%s/%s.png", getenv("HH_ICON_DIR"), tid);
    if (stat(cand, &sbuf) == 0) {
      snprintf(out, outsz, "%s", cand);
      return 0;
    }
  }
#endif
  /* 2. usual locations */
  static const char *const fmts[] = {
      "/user/appmeta/%s/icon0.png",
      "/user/appmeta/external/%s/icon0.png",
      "/system_ex/app/%s/sce_sys/icon0.png",
      "/system/vsh/app/%s/sce_sys/icon0.png",
      "/system_ex/vsh_asset/%s/icon0.png",
      "/user/app/%s/sce_sys/icon0.png",
  };
  for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
    snprintf(cand, sizeof(cand), fmts[i], tid);
    if (stat(cand, &sbuf) == 0) {
      snprintf(out, outsz, "%s", cand);
      return 0;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------------- */
/* diagnostics                                                               */
/* ------------------------------------------------------------------------- */

static void dump_rows(hh_t *h, strbuf_t *o, const char *sql, const char *a1,
                      const char *indent) {
  sqlite3_stmt *st = prep(h, sql, a1, NULL, NULL);
  if (!st) {
    sb_printf(o, "%s(query failed: %s)\n", indent, h->err);
    return;
  }
  int rows = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    rows++;
    sb_printf(o, "%s-", indent);
    int nc = sqlite3_column_count(st);
    for (int c = 0; c < nc; c++) {
      const char *name = sqlite3_column_name(st, c);
      const char *val = (const char *)sqlite3_column_text(st, c);
      if (sqlite3_column_type(st, c) == SQLITE_BLOB) val = "<blob>";
      char clip[161];
      snprintf(clip, sizeof(clip), "%s", val ? val : "NULL");
      sb_printf(o, " %s=%s%s", name, clip,
                (val && strlen(val) > 160) ? "..." : "");
    }
    sb_puts(o, "\n");
  }
  if (!rows) sb_printf(o, "%s(no rows)\n", indent);
  sqlite3_finalize(st);
}

static void dump_cols(hh_t *h, strbuf_t *o, const char *table) {
  sqlite3_stmt *st = prep(h,
      "SELECT name, type, pk FROM pragma_table_info(?1, 'main') ORDER BY cid",
      table, NULL, NULL);
  if (!st) return;
  sb_printf(o, "  %s columns:", table);
  while (sqlite3_step(st) == SQLITE_ROW)
    sb_printf(o, " %s(%s%s)", sqlite3_column_text(st, 0),
              sqlite3_column_text(st, 1), sqlite3_column_int(st, 2) ? ",pk" : "");
  sb_puts(o, "\n");
  sqlite3_finalize(st);
}

void hh_diag(hh_t *h, strbuf_t *o) {
  sb_printf(o, "Home Hider %s diagnostics\nSQLite %s\napp.db: %s\n", HH_VERSION,
            sqlite3_libversion(), h->app_db_path);
  if (!h->db) {
    sb_printf(o, "Database not open: %s\n", h->err);
    return;
  }
  sqlite3_stmt *st = prep(h, "PRAGMA main.journal_mode", NULL, NULL, NULL);
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW)
      sb_printf(o, "journal_mode: %s\n", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }

  sb_puts(o, "\nTables in app.db:\n");
  st = prep(h, "SELECT name FROM main.sqlite_master WHERE type='table' ORDER BY name",
            NULL, NULL, NULL);
  if (st) {
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW)
      sb_printf(o, "%s%s", n++ ? ", " : "  ", sqlite3_column_text(st, 0));
    sb_puts(o, "\n");
    sqlite3_finalize(st);
  }

  sb_printf(o, "\nUsers detected: %d\n", h->nusers);
  for (int i = 0; i < h->nusers; i++) {
    char t[96];
    sb_printf(o, " user %s (dispLocation:%s, MyHome group:%s)\n", h->users[i],
              h->has_disp[i] ? "yes" : "no", h->has_group[i] ? "yes" : "no");
    snprintf(t, sizeof(t), "tbl_iconinfo_%s", h->users[i]);
    dump_cols(h, o, t);
    if (h->has_group[i]) {
      snprintf(t, sizeof(t), "tbl_group_%s", h->users[i]);
      dump_cols(h, o, t);
    }
    snprintf(t, sizeof(t), "tbl_concepticoninfo_%s", h->users[i]);
    if (has_table(h, "main", t)) dump_cols(h, o, t);
  }
  if (has_table(h, "main", "tbl_contentinfo")) dump_cols(h, o, "tbl_contentinfo");

  for (int k = 0; k < HH_TARGET_COUNT; k++) {
    const char *tid = HH_TARGETS[k].title_id;
    sb_printf(o, "\n== %s (%s)\n", HH_TARGETS[k].name, tid);
    for (int i = 0; i < h->nusers; i++) {
      char sql[256];
      sb_printf(o, " user %s icon row:\n", h->users[i]);
      snprintf(sql, sizeof(sql),
               "SELECT * FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
               h->users[i]);
      dump_rows(h, o, sql, tid, "   ");
      if (h->has_group[i]) {
        sb_printf(o, " user %s MyHome rows:\n", h->users[i]);
        snprintf(sql, sizeof(sql),
                 "SELECT * FROM main.\"tbl_group_%s\" WHERE itemId=?1",
                 h->users[i]);
        dump_rows(h, o, sql, tid, "   ");
      }
    }
    if (has_table(h, "main", "tbl_contentinfo")) {
      sb_puts(o, " tbl_contentinfo:\n");
      dump_rows(h, o, "SELECT * FROM main.tbl_contentinfo WHERE titleId=?1", tid, "   ");
    }
  }

  for (int i = 0; i < h->nusers; i++) {
    if (!h->has_group[i]) continue;
    char sql[256];
    sb_printf(o, "\nMyHome group (user %s):\n", h->users[i]);
    snprintf(sql, sizeof(sql),
             "SELECT * FROM main.\"tbl_group_%s\" WHERE groupName='MyHome'",
             h->users[i]);
    dump_rows(h, o, sql, NULL, "   ");
  }

  sb_puts(o, "\nHome Hider hide list:\n");
  dump_rows(h, o, "SELECT * FROM hh.wanted", NULL, "   ");
  sb_puts(o, "Saved originals:\n");
  dump_rows(h, o, "SELECT * FROM hh.hidden", NULL, "   ");
}

/* ------------------------------------------------------------------------- */
/* backup / restore                                                          */
/* ------------------------------------------------------------------------- */

int hh_copy_db(const char *src, const char *dst, char *err, size_t errsz) {
  sqlite3 *s = NULL, *d = NULL;
  int ret = -1;
  if (sqlite3_open_v2(src, &s, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    snprintf(err, errsz, "open %s: %s", src, s ? sqlite3_errmsg(s) : "?");
    goto out;
  }
  if (sqlite3_open_v2(dst, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
      SQLITE_OK) {
    snprintf(err, errsz, "open %s: %s", dst, d ? sqlite3_errmsg(d) : "?");
    goto out;
  }
  sqlite3_busy_timeout(s, BUSY_TIMEOUT_MS);
  sqlite3_busy_timeout(d, BUSY_TIMEOUT_MS);
  sqlite3_backup *b = sqlite3_backup_init(d, "main", s, "main");
  if (!b) {
    snprintf(err, errsz, "backup init: %s", sqlite3_errmsg(d));
    goto out;
  }
  int rc, tries = 0;
  do {
    rc = sqlite3_backup_step(b, -1);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
      usleep(100000);
      tries++;
    }
  } while ((rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) &&
           tries < 100);
  sqlite3_backup_finish(b);
  if (rc != SQLITE_DONE) {
    snprintf(err, errsz, "backup step: %s", sqlite3_errstr(rc));
    goto out;
  }
  ret = 0;
out:
  if (s) sqlite3_close(s);
  if (d) sqlite3_close(d);
  return ret;
}

int hh_reset_state(const char *state_db, char *err, size_t errsz) {
  sqlite3 *d = NULL;
  if (sqlite3_open_v2(state_db, &d, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    if (d) sqlite3_close(d);
    return 0; /* nothing to reset */
  }
  char *msg = NULL;
  /* drop every table: hidden, wanted and grp_* backups */
  sqlite3_stmt *st = NULL;
  strbuf_t q;
  sb_init(&q);
  sb_puts(&q, "BEGIN;");
  if (sqlite3_prepare_v2(d, "SELECT name FROM sqlite_master WHERE type='table'",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *n = (const char *)sqlite3_column_text(st, 0);
      if (n && !strchr(n, '"')) sb_printf(&q, "DROP TABLE \"%s\";", n);
    }
    sqlite3_finalize(st);
  }
  sb_puts(&q, "COMMIT;");
  int rc = q.oom ? SQLITE_NOMEM : sqlite3_exec(d, q.data, NULL, NULL, &msg);
  if (rc != SQLITE_OK) snprintf(err, errsz, "reset: %s", msg ? msg : "?");
  sqlite3_free(msg);
  sb_free(&q);
  sqlite3_close(d);
  return rc == SQLITE_OK ? 0 : -1;
}
