#include "tabs.h"

#include <stdio.h>
#include <string.h>

#include "folders.h"
#include "log.h"

#define GAMES_FULL 0x8A /* dispLocation the console uses for a Games tab tile */
#define MEDIA_FULL 0xBC /* dispLocation the console uses for a Media tab tile */
#define CONTENT_KEY "__content__"

static void terr(hh_t *h, const char *msg) { snprintf(h->err, sizeof(h->err), "%s", msg); }

static sqlite3_stmt *prep(hh_t *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    snprintf(h->err, sizeof(h->err), "Database error: %s", sqlite3_errmsg(h->db));
    if (st) sqlite3_finalize(st);
    return NULL;
  }
  return st;
}

static int run(hh_t *h, const char *sql, const char *a, const char *b) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return -1;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    snprintf(h->err, sizeof(h->err), "Database error: %s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

static long long scalar1(hh_t *h, const char *sql, const char *a, long long dflt) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return dflt;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  long long v = dflt;
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static int has_col(hh_t *h, const char *table, const char *col) {
  sqlite3_stmt *st = prep(h, "SELECT count(*) FROM pragma_table_info(?1,'main') WHERE name=?2");
  if (!st) return 0;
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, col, -1, SQLITE_TRANSIENT);
  long long n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  return n > 0;
}

/* Saves tid's current dispLocation for user u, once. */
static void save_orig(hh_t *h, const char *tid, const char *u, long long disp) {
  sqlite3_stmt *st = prep(h, "INSERT OR IGNORE INTO hh.tab_orig(title_id,user,orig_disp) VALUES(?1,?2,?3)");
  if (!st) return;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, u, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, disp);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

/* Writes dispLocation = full for tid on every table, saving originals when
 * save is set. *changed = 1 if anything was written. */
static int set_tab(hh_t *h, const char *tid, int full, int save, int *changed) {
  *changed = 0;
  for (int i = 0; i < h->nusers; i++) {
    if (!h->has_disp[i]) continue;
    const char *u = h->users[i];
    char sql[220];
    snprintf(sql, sizeof(sql),
             "SELECT dispLocation FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1", u);
    long long cur = scalar1(h, sql, tid, -1);
    if (cur < 0) continue; /* tile not in this user's table */
    if (save) save_orig(h, tid, u, cur);
    snprintf(sql, sizeof(sql), "SELECT visible FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1", u);
    if (cur == full && scalar1(h, sql, tid, 0) == 1) continue; /* already placed and visible */
    snprintf(sql, sizeof(sql),
             "UPDATE main.\"tbl_iconinfo_%s\" SET visible=1, dispLocation=%d WHERE titleId=?1", u, full);
    if (run(h, sql, tid, NULL)) return -1;
    *changed = 1;
  }
  if (has_col(h, "tbl_contentinfo", "dispLocation")) {
    long long cur = scalar1(h, "SELECT dispLocation FROM main.tbl_contentinfo WHERE titleId=?1", tid, -1);
    if (cur >= 0) {
      if (save) save_orig(h, tid, CONTENT_KEY, cur);
      if (cur != full) {
        char sql[160];
        snprintf(sql, sizeof(sql), "UPDATE main.tbl_contentinfo SET dispLocation=%d WHERE titleId=?1", full);
        run(h, sql, tid, NULL);
        *changed = 1;
      }
    }
  }
  return 0;
}

static int tile_exists(hh_t *h, const char *tid) {
  for (int i = 0; i < h->nusers; i++) {
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
             h->users[i]);
    if (scalar1(h, sql, tid, 0) > 0) return 1;
  }
  return 0;
}

int tabs_move(hh_t *h, const char *tid, const char *dest) {
  if (h->nusers == 0) {
    terr(h, "This firmware's home screen database has a layout Home Hider doesn't know yet.");
    return -1;
  }
  if (!tid || !hh_valid_tid(tid)) {
    terr(h, "Missing or invalid title id.");
    return -1;
  }
  int games = dest && !strcmp(dest, "games");
  if (!games && (!dest || strcmp(dest, "media"))) {
    terr(h, "Choose the Games or Media tab.");
    return -1;
  }
  if (folders_is_folder(tid)) {
    terr(h, "Folders are moved in the folder editor, not here.");
    return -1;
  }
  if (hh_is_locked(tid)) {
    terr(h, "The PS5 decides this tile's tab, so it can't be moved.");
    return -1;
  }
  if (!tile_exists(h, tid)) {
    terr(h, "That app isn't on this console.");
    return -1;
  }
  if (scalar1(h, "SELECT count(*) FROM hh.folder_items WHERE title_id=?1", tid, 0) > 0) {
    terr(h, "This game is in a folder. Take it out of the folder first.");
    return -1;
  }
  if (scalar1(h, "SELECT count(*) FROM hh.wanted WHERE title_id=?1", tid, 0) > 0) {
    terr(h, "This tile is hidden. Show it first, then move it.");
    return -1;
  }

  if (run(h, "BEGIN IMMEDIATE", NULL, NULL)) return -1;
  int changed = 0, rc = set_tab(h, tid, games ? GAMES_FULL : MEDIA_FULL, 1, &changed);
  if (rc == 0) {
    sqlite3_stmt *st = prep(h, "INSERT INTO hh.tab_moved(title_id,dest,created_at) VALUES(?1,?2,datetime('now')) "
                               "ON CONFLICT(title_id) DO UPDATE SET dest=excluded.dest");
    if (st) {
      sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, games ? "games" : "media", -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) != SQLITE_DONE) rc = -1;
      sqlite3_finalize(st);
    } else {
      rc = -1;
    }
  }
  if (rc == 0) rc = run(h, "COMMIT", NULL, NULL);
  if (rc) {
    char keep[512];
    snprintf(keep, sizeof(keep), "%s", h->err);
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(h->err, sizeof(h->err), "%s", keep[0] ? keep : "Couldn't move the tile.");
    return -1;
  }
  hh_log("tab: moved %s to %s", tid, games ? "games" : "media");
  return 0;
}

int tabs_reset(hh_t *h, const char *tid) {
  if (!tid || !hh_valid_tid(tid)) {
    terr(h, "Missing or invalid title id.");
    return -1;
  }
  if (run(h, "BEGIN IMMEDIATE", NULL, NULL)) return -1;
  int rc = 0;
  sqlite3_stmt *st = prep(h, "SELECT user, orig_disp FROM hh.tab_orig WHERE title_id=?1");
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (rc == 0 && sqlite3_step(st) == SQLITE_ROW) {
      const char *u = (const char *)sqlite3_column_text(st, 0);
      long long disp = sqlite3_column_int64(st, 1);
      char sql[200];
      if (!strcmp(u, CONTENT_KEY)) {
        if (has_col(h, "tbl_contentinfo", "dispLocation")) {
          snprintf(sql, sizeof(sql), "UPDATE main.tbl_contentinfo SET dispLocation=%lld WHERE titleId=?1", disp);
          if (run(h, sql, tid, NULL)) rc = -1;
        }
      } else {
        snprintf(sql, sizeof(sql),
                 "UPDATE main.\"tbl_iconinfo_%s\" SET dispLocation=%lld WHERE titleId=?1", u, disp);
        if (run(h, sql, tid, NULL)) rc = -1;
      }
    }
    sqlite3_finalize(st);
  }
  if (rc == 0 && run(h, "DELETE FROM hh.tab_orig WHERE title_id=?1", tid, NULL)) rc = -1;
  if (rc == 0 && run(h, "DELETE FROM hh.tab_moved WHERE title_id=?1", tid, NULL)) rc = -1;
  if (rc == 0) rc = run(h, "COMMIT", NULL, NULL);
  if (rc) {
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    terr(h, "Couldn't put the tile back.");
    return -1;
  }
  hh_log("tab: %s back to its original tab", tid);
  return 0;
}

int tabs_apply(hh_t *h, int *fixed) {
  if (fixed) *fixed = 0;
  if (h->nusers == 0) return 0;
  if (scalar1(h, "SELECT count(*) FROM hh.tab_moved", NULL, 0) == 0) return 0;
  char ids[256][16], dests[256][8];
  int n = 0;
  sqlite3_stmt *st = prep(h, "SELECT title_id, dest FROM hh.tab_moved");
  if (!st) return -1;
  while (sqlite3_step(st) == SQLITE_ROW && n < 256) {
    snprintf(ids[n], 16, "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(dests[n], 8, "%s", (const char *)sqlite3_column_text(st, 1));
    n++;
  }
  sqlite3_finalize(st);
  if (run(h, "BEGIN IMMEDIATE", NULL, NULL)) return -1;
  int rc = 0;
  for (int i = 0; i < n && rc == 0; i++) {
    /* A tile the user has hidden must stay hidden: re-applying its tab move
     * would set visible=1 and bring it back. Skip it while it's on the hide
     * list; it resumes moving when shown again. */
    if (scalar1(h, "SELECT count(*) FROM hh.wanted WHERE title_id=?1", ids[i], 0) > 0) continue;
    int changed = 0;
    rc = set_tab(h, ids[i], strcmp(dests[i], "media") ? GAMES_FULL : MEDIA_FULL, 0, &changed);
    if (rc == 0 && changed && fixed) (*fixed)++;
  }
  if (rc == 0) rc = run(h, "COMMIT", NULL, NULL);
  if (rc) sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
  return rc;
}

int tabs_moved_json(hh_t *h, strbuf_t *sb) {
  sb_puts(sb, "[");
  sqlite3_stmt *st = prep(h, "SELECT title_id, dest FROM hh.tab_moved");
  int first = 1;
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      sb_printf(sb, "%s{\"id\":\"%s\",\"dest\":\"%s\"}", first ? "" : ",",
                (const char *)sqlite3_column_text(st, 0), (const char *)sqlite3_column_text(st, 1));
      first = 0;
    }
    sqlite3_finalize(st);
  }
  sb_puts(sb, "]");
  return sb->oom ? -1 : 0;
}
