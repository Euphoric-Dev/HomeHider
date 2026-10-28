/* Home Hider - home screen tile hiding logic (PS5 app.db).
 *
 * How a tile is hidden (per console user <u>):
 *   1. tbl_iconinfo_<u>.visible       -> 0
 *   2. tbl_iconinfo_<u>.dispLocation  -> Games (0x02) and Media (0x04) bits cleared
 *   3. tbl_group_<u> 'MyHome' row     -> removed (this pins system tiles such
 *                                        as the PS Store to the Games row)
 * Every original value / removed row is saved in our own state.db first, so
 * "Show" puts back exactly what was there. Nothing is ever deleted from the
 * console besides the MyHome pin row, which is backed up and restored.
 */
#pragma once

#include "strbuf.h"
#include "third_party/sqlite/sqlite3.h"

#define HH_VERSION      "2.1.0"
#define HH_MAX_USERS    16
#define HH_USER_LEN     48
#define HH_BIT_GAMES    0x02
#define HH_BIT_MEDIA    0x04

typedef struct {
  const char *title_id;
  const char *name;
  const char *tab;
} hh_target_t;

/* Sample tiles included in diagnostics output. */
extern const hh_target_t HH_TARGETS[];
extern const int HH_TARGET_COUNT;

typedef struct {
  sqlite3 *db;                         /* app.db as "main", state.db as "hh" */
  int      nusers;
  char     users[HH_MAX_USERS][HH_USER_LEN];
  int      has_disp[HH_MAX_USERS];     /* tbl_iconinfo_<u>.dispLocation exists */
  int      has_group[HH_MAX_USERS];    /* tbl_group_<u>(itemId, groupName) exists */
  char     app_db_path[256];
  char     err[512];
} hh_t;

typedef struct {
  int present;      /* found in at least one user's tables */
  int visible;      /* visible != 0 for at least one user */
  int in_myhome;    /* pinned in at least one user's MyHome group */
  int wanted;       /* Home Hider has this title on its hide list */
  int disp;         /* dispLocation of first user that has the title, -1 if none */
} hh_state_t;

/* Returns 1 if tid looks like a PlayStation title id (e.g. NPXS40047). */
int  hh_valid_tid(const char *tid);

/* Opens app.db (must exist) and attaches/creates state.db. Returns 0 on
 * success. An unknown layout (no tbl_iconinfo_* tables) still opens so that
 * diagnostics work; hide/unhide then fail with a clear message. */
int  hh_open(hh_t *h, const char *app_db, const char *state_db);
void hh_close(hh_t *h);

int  hh_hide(hh_t *h, const char *tid);
int  hh_unhide(hh_t *h, const char *tid);
int  hh_unhide_all(hh_t *h, int *count);
/* Re-hides every title on the hide list that the system has made visible
 * again. *fixed receives how many titles needed fixing. */
int  hh_reapply(hh_t *h, int *fixed);
int  hh_get_state(hh_t *h, const char *tid, hh_state_t *st);

/* Appends a JSON array describing every installed game/app and every system
 * tile that is on (or was hidden from) the home screen. Each element:
 *   {"id","name","kind":"game|system","tab":"games|media|none",
 *    "hidden","on_list","pinned","recent","folder","renamed","custom_icon"} */
int  hh_list(hh_t *h, strbuf_t *out);

/* Name the home screen shows for tid ("" if unknown). */
void hh_title_name(hh_t *h, const char *tid, char *out, size_t n);

/* Finds a safe local icon file for tid (png/jpg under system/app folders).
 * Returns 0 and fills out on success. */
int  hh_icon_path(hh_t *h, const char *tid, char *out, size_t outsz);

/* Placeholder/internal titles that never appear as real tiles. */
int  hh_is_internal(const char *tid);
/* Tiles the home screen always adds itself (database hiding has no effect). */
int  hh_is_locked(const char *tid);
/* Shows again any locked tile an older version hid. *count = how many. */
int  hh_release_locked(hh_t *h, int *count);
void hh_diag(hh_t *h, strbuf_t *out);

/* Online copy of one SQLite file to another using the backup API. */
int  hh_copy_db(const char *src, const char *dst, char *err, size_t errsz);
/* Clears the hide list and saved originals (used after restoring a backup). */
int  hh_reset_state(const char *state_db, char *err, size_t errsz);
