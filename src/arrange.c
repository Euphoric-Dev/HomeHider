#include "arrange.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* lastAccessIndex values for arranged tiles sit this far above the console's
 * counter, so they stay in front and normal play can't catch up. */
#define ARRANGE_BASE_GAP 1000000
#define ARRANGE_MAX      512

static void aerr(hh_t *h, const char *msg) { snprintf(h->err, sizeof(h->err), "%s", msg); }

static sqlite3_stmt *prep(hh_t *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    snprintf(h->err, sizeof(h->err), "Database error: %s", sqlite3_errmsg(h->db));
    if (st) sqlite3_finalize(st);
    return NULL;
  }
  return st;
}

/* Runs sql with up to three bound text args. 0 on success. */
static int run(hh_t *h, const char *sql, const char *a, const char *b, const char *c) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return -1;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
  if (c) sqlite3_bind_text(st, 3, c, -1, SQLITE_TRANSIENT);
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

static long long scalar2(hh_t *h, const char *sql, const char *a, const char *b, long long dflt) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return dflt;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
  long long v = dflt;
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static int has_col(hh_t *h, const char *table, const char *col) {
  sqlite3_stmt *st = prep(h, "SELECT count(*) FROM pragma_table_info(?1, 'main') WHERE name=?2");
  if (!st) return 0;
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, col, -1, SQLITE_TRANSIENT);
  long long n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  return n > 0;
}

/* ------------------------------------------------------------------------- */
/* MyHome (pinning)                                                          */
/* ------------------------------------------------------------------------- */

static int myhome_count(hh_t *h, const char *u, const char *tid) {
  char sql[180];
  snprintf(sql, sizeof(sql),
           "SELECT count(*) FROM main.\"tbl_group_%s\" WHERE itemId=?1 AND groupName='MyHome'", u);
  return (int)scalar1(h, sql, tid, 0);
}

/* Current MyHome idx of a tile, or -1 if not pinned / no idx column. */
static long long myhome_idx(hh_t *h, const char *u, const char *tid) {
  char tbl[64];
  snprintf(tbl, sizeof(tbl), "tbl_group_%s", u);
  if (!has_col(h, tbl, "idx")) return -1;
  char sql[200];
  snprintf(sql, sizeof(sql),
           "SELECT idx FROM main.\"%s\" WHERE itemId=?1 AND groupName='MyHome'", tbl);
  return scalar1(h, sql, tid, -1);
}

/* Put a pinned tile back at a specific idx (used to undo a reorder). */
static void myhome_set_idx(hh_t *h, const char *u, const char *tid, long long idx) {
  char tbl[64];
  snprintf(tbl, sizeof(tbl), "tbl_group_%s", u);
  if (idx < 0 || !has_col(h, tbl, "idx")) return;
  char sql[220];
  snprintf(sql, sizeof(sql),
           "UPDATE main.\"%s\" SET idx=%lld WHERE itemId=?1 AND groupName='MyHome'", tbl, idx);
  run(h, sql, tid, NULL, NULL);
}

static void myhome_add(hh_t *h, const char *u, const char *tid, int pos) {
  char tbl[64];
  snprintf(tbl, sizeof(tbl), "tbl_group_%s", u);
  if (myhome_count(h, u, tid)) {
    if (has_col(h, tbl, "idx")) {
      char sql[220];
      snprintf(sql, sizeof(sql),
               "UPDATE main.\"%s\" SET idx=%d WHERE itemId=?1 AND groupName='MyHome'", tbl, pos);
      run(h, sql, tid, NULL, NULL);
    }
    return;
  }
  char cols[200] = "itemId, groupName", vals[200] = "?1, 'MyHome'";
  if (has_col(h, tbl, "type")) {
    strncat(cols, ", type", sizeof(cols) - strlen(cols) - 1);
    strncat(vals, ", 'title'", sizeof(vals) - strlen(vals) - 1);
  }
  if (has_col(h, tbl, "idx")) {
    char p[24];
    snprintf(p, sizeof(p), ", %d", pos);
    strncat(cols, ", idx", sizeof(cols) - strlen(cols) - 1);
    strncat(vals, p, sizeof(vals) - strlen(vals) - 1);
  }
  if (has_col(h, tbl, "lastModifiedDate")) {
    strncat(cols, ", lastModifiedDate", sizeof(cols) - strlen(cols) - 1);
    strncat(vals, ", datetime('now')", sizeof(vals) - strlen(vals) - 1);
  }
  char sql[480];
  snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO main.\"%s\"(%s) VALUES(%s)", tbl, cols, vals);
  run(h, sql, tid, NULL, NULL);
}

static void myhome_remove(hh_t *h, const char *u, const char *tid) {
  char sql[180];
  snprintf(sql, sizeof(sql),
           "DELETE FROM main.\"tbl_group_%s\" WHERE itemId=?1 AND groupName='MyHome'", u);
  run(h, sql, tid, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static const char *tile_tab(hh_t *h, const char *tid) {
  for (int ui = 0; ui < h->nusers; ui++) {
    if (!h->has_disp[ui]) continue;
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT dispLocation FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
             h->users[ui]);
    long long d = scalar1(h, sql, tid, -1);
    if (d >= 0) {
      if (d & HH_BIT_GAMES) return "games";
      if (d & HH_BIT_MEDIA) return "media";
      return "none";
    }
  }
  return "none";
}

static int tile_exists(hh_t *h, const char *tid) {
  for (int ui = 0; ui < h->nusers; ui++) {
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
             h->users[ui]);
    if (scalar1(h, sql, tid, 0) > 0) return 1;
  }
  return 0;
}

/* Puts one tile back the way it was, and forgets its saved originals. */
static void restore_tile(hh_t *h, const char *tid) {
  for (int ui = 0; ui < h->nusers; ui++) {
    const char *u = h->users[ui];
    char icon_t[64];
    snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", u);
    sqlite3_stmt *st = prep(h, "SELECT orig_access, orig_pinned, orig_idx FROM hh.arrange_orig "
                               "WHERE title_id=?1 AND user=?2");
    if (!st) continue;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, u, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
      sqlite3_finalize(st);
      continue;
    }
    int acc_null = sqlite3_column_type(st, 0) == SQLITE_NULL;
    long long acc = acc_null ? 0 : sqlite3_column_int64(st, 0);
    int was_pinned = sqlite3_column_int(st, 1);
    long long orig_idx = sqlite3_column_type(st, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(st, 2);
    sqlite3_finalize(st);
    if (!acc_null && has_col(h, icon_t, "lastAccessIndex")) {
      char sql[220];
      snprintf(sql, sizeof(sql), "UPDATE main.\"%s\" SET lastAccessIndex=%lld WHERE titleId=?1",
               icon_t, acc);
      run(h, sql, tid, NULL, NULL);
    }
    if (!was_pinned && h->has_group[ui]) myhome_remove(h, u, tid);
    else if (was_pinned && h->has_group[ui]) myhome_set_idx(h, u, tid, orig_idx); /* put its pin position back */
  }
  run(h, "DELETE FROM hh.arrange_orig WHERE title_id=?1", tid, NULL, NULL);
}

/* Writes the order and pins from hh.arranged into app.db. */
static int apply_locked(hh_t *h, int *fixed) {
  if (fixed) *fixed = 0;
  long long maxacc = 0;
  for (int ui = 0; ui < h->nusers; ui++) {
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT max(lastAccessIndex) FROM main.\"tbl_iconinfo_%s\"",
             h->users[ui]);
    long long m = scalar1(h, sql, NULL, 0);
    if (m > maxacc) maxacc = m;
  }
  long long ver = scalar1(h, "SELECT status FROM main.tbl_version WHERE category='access_index'",
                          NULL, 0);
  long long base = (ver > maxacc ? ver : maxacc) + ARRANGE_BASE_GAP;

  sqlite3_stmt *st = prep(h, "SELECT title_id, pos, pinned FROM hh.arranged ORDER BY pos");
  if (!st) return -1;
  int rc = 0;
  while (rc == 0 && sqlite3_step(st) == SQLITE_ROW) {
    char tid[16];
    snprintf(tid, sizeof(tid), "%s", (const char *)sqlite3_column_text(st, 0));
    /* leave a hidden tile alone: re-pinning it could bring it back on screen */
    if (scalar1(h, "SELECT count(*) FROM hh.wanted WHERE title_id=?1", tid, 0) > 0) continue;
    if (hh_is_locked(tid)) continue; /* the PS5 places Game/App Library itself */
    int pos = sqlite3_column_int(st, 1), pinned = sqlite3_column_int(st, 2);
    long long want = base - pos;
    for (int ui = 0; ui < h->nusers; ui++) {
      const char *u = h->users[ui];
      char icon_t[64];
      snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", u);
      char csql[180];
      snprintf(csql, sizeof(csql), "SELECT count(*) FROM main.\"%s\" WHERE titleId=?1", icon_t);
      if (scalar1(h, csql, tid, 0) == 0) continue;

      int cur_pin = h->has_group[ui] ? (myhome_count(h, u, tid) > 0) : 0;
      long long cur_idx = cur_pin ? myhome_idx(h, u, tid) : -1;
      long long cur_acc = -1;
      if (has_col(h, icon_t, "lastAccessIndex")) {
        char asql[200];
        snprintf(asql, sizeof(asql), "SELECT lastAccessIndex FROM main.\"%s\" WHERE titleId=?1",
                 icon_t);
        cur_acc = scalar1(h, asql, tid, -1);
      }
      /* save the true originals once */
      sqlite3_stmt *ins = prep(h, "INSERT OR IGNORE INTO hh.arrange_orig"
                                  "(title_id, user, orig_access, orig_pinned, orig_idx) VALUES(?1,?2,?3,?4,?5)");
      if (ins) {
        sqlite3_bind_text(ins, 1, tid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, u, -1, SQLITE_TRANSIENT);
        if (cur_acc >= 0) sqlite3_bind_int64(ins, 3, cur_acc);
        else sqlite3_bind_null(ins, 3);
        sqlite3_bind_int(ins, 4, cur_pin);
        if (cur_idx >= 0) sqlite3_bind_int64(ins, 5, cur_idx);
        else sqlite3_bind_null(ins, 5);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
      }
      if (has_col(h, icon_t, "lastAccessIndex")) {
        char sql[220];
        snprintf(sql, sizeof(sql), "UPDATE main.\"%s\" SET lastAccessIndex=%lld WHERE titleId=?1",
                 icon_t, want);
        if (run(h, sql, tid, NULL, NULL)) {
          rc = -1;
          break;
        }
      }
      if (h->has_group[ui]) {
        /* The home screen orders pinned (MyHome) tiles by their idx, not by
         * lastAccessIndex, so a tile the PS5 keeps pinned (Game Library, Store)
         * only moves when we set its idx. Keep every already-pinned tile, and
         * any the user explicitly pins, in the arranged order. */
        if (pinned || cur_pin) {
          myhome_add(h, u, tid, pos);
        } else {
          int orig = (int)scalar2(h,
              "SELECT orig_pinned FROM hh.arrange_orig WHERE title_id=?1 AND user=?2", tid, u, 1);
          if (!orig) myhome_remove(h, u, tid); /* undo a pin we added */
        }
      }
    }
    if (fixed) (*fixed)++;
  }
  sqlite3_finalize(st);
  return rc;
}

/* ------------------------------------------------------------------------- */
/* public                                                                    */
/* ------------------------------------------------------------------------- */

static int in_csv(const char *csv, const char *tid) {
  size_t n = strlen(tid);
  const char *p = csv;
  while (p && *p) {
    const char *c = strchr(p, ',');
    size_t len = c ? (size_t)(c - p) : strlen(p);
    if (len == n && strncmp(p, tid, n) == 0) return 1;
    p = c ? c + 1 : NULL;
  }
  return 0;
}

int arrange_set(hh_t *h, const char *items_csv, const char *pinned_csv) {
  if (h->nusers == 0) {
    aerr(h, "This firmware's home screen database has a layout Home Hider doesn't know yet.");
    return -1;
  }
  if (!items_csv) items_csv = "";
  if (!pinned_csv) pinned_csv = "";
  if (run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL)) return -1;

  int rc = 0;
  /* restore tiles that drop out of the new order */
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.arranged");
  if (st) {
    char drop[ARRANGE_MAX][16];
    int nd = 0;
    while (sqlite3_step(st) == SQLITE_ROW && nd < ARRANGE_MAX) {
      const char *t = (const char *)sqlite3_column_text(st, 0);
      if (t && !in_csv(items_csv, t)) snprintf(drop[nd++], 16, "%s", t);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < nd; i++) restore_tile(h, drop[i]);
  }
  rc = run(h, "DELETE FROM hh.arranged", NULL, NULL, NULL);

  /* insert the new order */
  int pos = 0;
  const char *p = items_csv;
  while (rc == 0 && p && *p && pos < ARRANGE_MAX) {
    const char *c = strchr(p, ',');
    size_t len = c ? (size_t)(c - p) : strlen(p);
    char tid[16] = "";
    if (len > 0 && len < sizeof(tid)) {
      memcpy(tid, p, len);
      tid[len] = '\0';
    }
    p = c ? c + 1 : NULL;
    /* Game/App Library are placed by the PS5, not by lastAccessIndex or MyHome
     * idx, so they can't be reordered; leave them out of the arrangement. */
    if (!hh_valid_tid(tid) || hh_is_internal(tid) || hh_is_locked(tid) || !tile_exists(h, tid)) continue;
    /* skip a duplicate already inserted */
    if (scalar1(h, "SELECT count(*) FROM hh.arranged WHERE title_id=?1", tid, 0) > 0) continue;
    int pinned = in_csv(pinned_csv, tid) ? 1 : 0;
    sqlite3_stmt *ins = prep(h, "INSERT INTO hh.arranged(title_id, tab, pos, pinned, updated_at) "
                                "VALUES(?1,?2,?3,?4,datetime('now'))");
    if (!ins) {
      rc = -1;
      break;
    }
    sqlite3_bind_text(ins, 1, tid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, tile_tab(h, tid), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins, 3, pos);
    sqlite3_bind_int(ins, 4, pinned);
    if (sqlite3_step(ins) != SQLITE_DONE) rc = -1;
    sqlite3_finalize(ins);
    pos++;
  }

  int fixed = 0;
  if (rc == 0) rc = apply_locked(h, &fixed);
  if (rc == 0) rc = run(h, "COMMIT", NULL, NULL, NULL);
  if (rc) {
    char keep[512];
    snprintf(keep, sizeof(keep), "%s", h->err);
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(h->err, sizeof(h->err), "%s", keep[0] ? keep : "Couldn't save the arrangement.");
    return -1;
  }
  hh_log("arrange: %d tile(s) ordered", fixed);
  return 0;
}

int arrange_clear(hh_t *h) {
  if (run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL)) return -1;
  int rc = 0;
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.arrange_orig GROUP BY title_id");
  if (st) {
    char tids[ARRANGE_MAX][16];
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < ARRANGE_MAX) {
      const char *t = (const char *)sqlite3_column_text(st, 0);
      if (t) snprintf(tids[n++], 16, "%s", t);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++) restore_tile(h, tids[i]);
  }
  if (rc == 0) rc = run(h, "DELETE FROM hh.arranged", NULL, NULL, NULL);
  if (rc == 0) rc = run(h, "COMMIT", NULL, NULL, NULL);
  if (rc) {
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
  }
  hh_log("arrange: cleared");
  return 0;
}

int arrange_apply(hh_t *h, int *fixed) {
  if (fixed) *fixed = 0;
  if (h->nusers == 0) return 0;
  if (scalar1(h, "SELECT count(*) FROM hh.arranged", NULL, 0) == 0) return 0;
  if (run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL)) return -1;
  int rc = apply_locked(h, fixed);
  if (rc == 0) rc = run(h, "COMMIT", NULL, NULL, NULL);
  if (rc) sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
  return rc;
}

int arrange_json(hh_t *h, strbuf_t *sb) {
  int on = scalar1(h, "SELECT count(*) FROM hh.arranged", NULL, 0) > 0;
  sb_printf(sb, "{\"on\":%s,\"items\":[", on ? "true" : "false");
  sqlite3_stmt *st = prep(h, "SELECT title_id, tab, pinned FROM hh.arranged ORDER BY pos");
  int first = 1;
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      sb_printf(sb, "%s{\"id\":\"%s\",\"tab\":\"%s\",\"pinned\":%s}", first ? "" : ",",
                (const char *)sqlite3_column_text(st, 0), (const char *)sqlite3_column_text(st, 1),
                sqlite3_column_int(st, 2) ? "true" : "false");
      first = 0;
    }
    sqlite3_finalize(st);
  }
  sb_puts(sb, "]}");
  return sb->oom ? -1 : 0;
}
