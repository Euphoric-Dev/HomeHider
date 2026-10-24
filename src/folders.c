#include "folders.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "icons.h"
#include "texture.h"
#include "log.h"
#include "platform.h"

static struct {
  char data_dir[256];
  char app_dir[256];
  int port;
} F;

#define GAMES_FULL 0x8A /* typical dispLocation of a Games tab tile */
#define MEDIA_FULL 0xBC /* typical dispLocation of a Media tab tile */

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void ferr(hh_t *h, const char *fmt, const char *a) {
  snprintf(h->err, sizeof(h->err), fmt, a ? a : "");
}

static sqlite3_stmt *prep(hh_t *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
    return NULL;
  }
  return st;
}

static int run1(hh_t *h, const char *sql, const char *a, const char *b) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return -1;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

static long long count1(hh_t *h, const char *sql, const char *a) {
  sqlite3_stmt *st = prep(h, sql);
  long long n = 0;
  if (!st) return 0;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return n;
}

static int col_exists(hh_t *h, const char *table, const char *col) {
  sqlite3_stmt *st = prep(h, "SELECT count(*) FROM pragma_table_info(?1, 'main') WHERE name=?2");
  int n = 0;
  if (!st) return 0;
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, col, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return n > 0;
}

int folders_is_folder(const char *tid) {
  return tid && strncmp(tid, FOLDER_PREFIX, 4) == 0 && hh_valid_tid(tid);
}

static int write_file(const char *path, const void *data, size_t len) {
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  if (!ok || rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  chmod(path, 0777);
  return 0;
}

static int read_file(const char *path, unsigned char **out, size_t *len) {
  struct stat st;
  if (stat(path, &st) || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  *out = malloc((size_t)st.st_size);
  if (!*out) {
    fclose(f);
    return -1;
  }
  *len = fread(*out, 1, (size_t)st.st_size, f);
  fclose(f);
  if (*len != (size_t)st.st_size) {
    free(*out);
    return -1;
  }
  return 0;
}

static void mkdirs(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  mkdir(tmp, 0777);
}

void folders_init(const folders_cfg_t *cfg) {
  snprintf(F.data_dir, sizeof(F.data_dir), "%s", cfg->data_dir);
  snprintf(F.app_dir, sizeof(F.app_dir), "%s", cfg->app_dir);
  F.port = cfg->port;
}

static void custom_icon_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/folders/%s.png", F.data_dir, tid);
}

/* the background as the tile uses it (BC7 DDS) and the picture (JPEG) */
static void bg_dds_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/folders/%s_pic0.dds", F.data_dir, tid);
}

static void bg_jpg_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/folders/%s_bg.jpg", F.data_dir, tid);
}

int folders_bg_path(const char *tid, char *out, size_t outsz) {
  if (!folders_is_folder(tid)) return -1;
  bg_jpg_path(tid, out, outsz);
  struct stat st;
  return stat(out, &st) == 0 ? 0 : -1;
}

int folders_icon_path(const char *tid, char *out, size_t outsz) {
  if (!folders_is_folder(tid)) return -1;
  snprintf(out, outsz, "%s/%s/sce_sys/icon0.png", F.app_dir, tid);
  struct stat st;
  return stat(out, &st) == 0 ? 0 : -1;
}

/* Name + recency of a title as the home screen shows it. */
static void title_info(hh_t *h, const char *tid, char *name, size_t namesz, long long *recent) {
  name[0] = '\0';
  *recent = 0;
  char sql[600];
  for (int i = 0; i < h->nusers && !name[0]; i++) {
    char icon_t[96], con_t[96];
    snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", h->users[i]);
    snprintf(con_t, sizeof(con_t), "tbl_concepticoninfo_%s", h->users[i]);
    int has_con = col_exists(h, con_t, "conceptName") && col_exists(h, icon_t, "localConceptId");
    int has_name = col_exists(h, icon_t, "titleName");
    int has_acc = col_exists(h, icon_t, "lastAccessIndex");
    snprintf(sql, sizeof(sql),
             "SELECT %s, %s FROM main.\"%s\" i %s%s%s WHERE i.titleId=?1 LIMIT 1",
             has_con ? (has_name ? "coalesce(nullif(c.conceptName,''), i.titleName)" : "c.conceptName")
                     : (has_name ? "i.titleName" : "NULL"),
             has_acc ? "i.lastAccessIndex" : "0", icon_t,
             has_con ? "LEFT JOIN main.\"" : "", has_con ? con_t : "",
             has_con ? "\" c ON c.localConceptId = i.localConceptId" : "");
    sqlite3_stmt *st = prep(h, sql);
    if (!st) continue;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
      const char *n = (const char *)sqlite3_column_text(st, 0);
      if (n) snprintf(name, namesz, "%s", n);
      *recent = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
  }
  if (!name[0]) snprintf(name, namesz, "%s", tid);
}

static int tile_present(hh_t *h, const char *tid) {
  char sql[200];
  for (int i = 0; i < h->nusers; i++) {
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
             h->users[i]);
    if (count1(h, sql, tid) > 0) return 1;
  }
  return 0;
}

/* Puts the folder tile on the chosen tab and makes it visible.
 * Returns 1 if something had to change. */
static int place_tile(hh_t *h, const char *tid, const char *tab) {
  int games = strcmp(tab, "media") != 0;
  int set = games ? HH_BIT_GAMES : HH_BIT_MEDIA, clr = games ? HH_BIT_MEDIA : HH_BIT_GAMES;
  int full = games ? GAMES_FULL : MEDIA_FULL;
  char sql[400], expr[200];
  snprintf(expr, sizeof(expr),
           "CASE WHEN (dispLocation & ~6) = 0 THEN %d ELSE ((dispLocation | %d) & ~%d) END",
           full, set, clr);
  int changed = 0;
  for (int i = 0; i < h->nusers; i++) {
    const char *u = h->users[i];
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1 AND "
             "(visible = 0%s)",
             u, h->has_disp[i] ? games ? " OR (dispLocation & 2) = 0 OR (dispLocation & 4) != 0"
                                       : " OR (dispLocation & 4) = 0 OR (dispLocation & 2) != 0"
                               : "");
    if (count1(h, sql, tid) == 0) continue;
    changed = 1;
    if (h->has_disp[i])
      snprintf(sql, sizeof(sql),
               "UPDATE main.\"tbl_iconinfo_%s\" SET visible=1, dispLocation=%s WHERE titleId=?1", u, expr);
    else
      snprintf(sql, sizeof(sql), "UPDATE main.\"tbl_iconinfo_%s\" SET visible=1 WHERE titleId=?1", u);
    run1(h, sql, tid, NULL);
  }
  if (changed && col_exists(h, "tbl_contentinfo", "dispLocation")) {
    snprintf(sql, sizeof(sql), "UPDATE main.tbl_contentinfo SET dispLocation=%s WHERE titleId=?1", expr);
    run1(h, sql, tid, NULL);
  }
  return changed;
}

/* ------------------------------------------------------------------------- */
/* tile files                                                                */
/* ------------------------------------------------------------------------- */

static int write_tile(hh_t *h, const char *tid) {
  char name[FOLDER_NAME_MAX * 2] = "", color[16] = "blue";
  int custom = 0;
  sqlite3_stmt *st = prep(h, "SELECT name, color, custom_icon FROM hh.folders WHERE title_id=?1");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(name, sizeof(name), "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(color, sizeof(color), "%s", (const char *)sqlite3_column_text(st, 1));
    custom = sqlite3_column_int(st, 2);
  }
  sqlite3_finalize(st);
  if (!name[0]) {
    ferr(h, "Folder %s not found.", tid);
    return -1;
  }

  char dir[400], path[520];
  snprintf(dir, sizeof(dir), "%s/%s/sce_sys", F.app_dir, tid);
  mkdirs(dir);

  /* param.json: a launcher whose "game" is our folder page */
  strbuf_t pj;
  sb_init(&pj);
  sb_printf(&pj, "{\n  \"titleId\": \"%s\",\n  \"applicationCategoryType\": 65536,\n"
                 "  \"deeplinkUri\": \"http://127.0.0.1:%d/folder/%s\",\n"
                 "  \"localizedParameters\": {\n    \"defaultLanguage\": \"en-US\",\n"
                 "    \"en-US\": {\n      \"titleName\": ",
            tid, F.port, tid);
  sb_json_str(&pj, name);
  sb_puts(&pj, "\n    }\n  }\n}\n");
  snprintf(path, sizeof(path), "%s/param.json", dir);
  int rc = pj.oom ? -1 : write_file(path, pj.data, pj.len);
  sb_free(&pj);
  if (rc) {
    ferr(h, "Couldn't write %.400s.", path);
    return -1;
  }

  /* icon0.png: custom picture or a collage of the first four games */
  unsigned char *png = NULL;
  size_t png_len = 0;
  char cpath[400];
  custom_icon_path(tid, cpath, sizeof(cpath));
  unsigned char *raw = NULL;
  size_t raw_len = 0;
  if (custom && read_file(cpath, &raw, &raw_len) == 0) {
    /* a transparent picture goes on the folder's colour */
    if (icon_flatten(raw, raw_len, color, &png, &png_len)) png = NULL;
    free(raw);
  }
  if (!png) {
    char paths[4][512];
    const char *ptrs[4] = {NULL, NULL, NULL, NULL};
    int n = 0;
    st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 ORDER BY pos LIMIT 4");
    if (st) {
      sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
      while (sqlite3_step(st) == SQLITE_ROW && n < 4) {
        const char *it = (const char *)sqlite3_column_text(st, 0);
        if (it && hh_icon_path(h, it, paths[n], sizeof(paths[n])) == 0) ptrs[n] = paths[n];
        n++;
      }
      sqlite3_finalize(st);
    }
    if (icon_collage(ptrs, n, color, &png, &png_len)) {
      ferr(h, "Couldn't draw the folder icon.%s", "");
      return -1;
    }
  }
  snprintf(path, sizeof(path), "%s/icon0.png", dir);
  rc = write_file(path, png, png_len);
  free(png);
  if (rc) {
    ferr(h, "Couldn't write %.400s.", path);
    return -1;
  }

  /* pic0.dds: the background, if the folder has one */
  char bg[400];
  bg_dds_path(tid, bg, sizeof(bg));
  snprintf(path, sizeof(path), "%s/pic0.dds", dir);
  unsigned char *dds = NULL;
  size_t dds_len = 0;
  if (read_file(bg, &dds, &dds_len) == 0) {
    rc = write_file(path, dds, dds_len);
    free(dds);
    if (rc) {
      ferr(h, "Couldn't write %.400s.", path);
      return -1;
    }
  } else {
    unlink(path);
  }
  return 0;
}

/* Writes the files, registers the tile and puts it on its tab. */
static int install_tile(hh_t *h, const char *tid, char *note, size_t notesz) {
  if (write_tile(h, tid)) {
    hh_log("folder %s: writing tile files failed: %s", tid, h->err);
    return -1;
  }
  hh_log("folder %s: tile files written to %s/%s", tid, F.app_dir, tid);
  int err = plat_app_install(tid, F.app_dir);
  hh_log("folder %s: app install returned 0x%08X", tid, (unsigned)err);
  if (err) {
    char msg[64];
    snprintf(msg, sizeof(msg), "0x%08X", (unsigned)err);
    ferr(h, "The console refused to add the folder tile (error %s).", msg);
    return -1;
  }
  char tab[16] = "games";
  sqlite3_stmt *st = prep(h, "SELECT tab FROM hh.folders WHERE title_id=?1");
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(tab, sizeof(tab), "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  /* the system adds the tile to app.db shortly after installing */
  int waited = 0;
  for (; waited < 40 && !tile_present(h, tid); waited++) usleep(200000);
  hh_log("folder %s: tile in app.db=%d after %d ms", tid, tile_present(h, tid), waited * 200);
  if (!tile_present(h, tid)) {
    if (note) snprintf(note, notesz, "The tile was installed but hasn't appeared yet. Restart the PS5 if it doesn't show up.");
    return 0;
  }
  int moved = place_tile(h, tid, tab);
  hh_log("folder %s: placed on %s tab (changed=%d)", tid, tab, moved);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* JSON                                                                      */
/* ------------------------------------------------------------------------- */

static void folder_json(hh_t *h, const char *tid, strbuf_t *sb) {
  sqlite3_stmt *st = prep(h, "SELECT name, tab, color, custom_icon FROM hh.folders WHERE title_id=?1");
  if (!st) return;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    return;
  }
  sb_printf(sb, "{\"id\":\"%s\",\"name\":", tid);
  sb_json_str(sb, (const char *)sqlite3_column_text(st, 0));
  sb_puts(sb, ",\"tab\":");
  sb_json_str(sb, (const char *)sqlite3_column_text(st, 1));
  sb_puts(sb, ",\"color\":");
  sb_json_str(sb, (const char *)sqlite3_column_text(st, 2));
  sb_printf(sb, ",\"custom_icon\":%s", sqlite3_column_int(st, 3) ? "true" : "false");
  sqlite3_finalize(st);
  char bgp[400];
  sb_printf(sb, ",\"custom_bg\":%s", folders_bg_path(tid, bgp, sizeof(bgp)) == 0 ? "true" : "false");
  sb_printf(sb, ",\"installed\":%s,\"items\":[", tile_present(h, tid) ? "true" : "false");

  st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 ORDER BY pos");
  int first = 1;
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *it = (const char *)sqlite3_column_text(st, 0);
      char name[200];
      long long recent;
      title_info(h, it, name, sizeof(name), &recent);
      sb_printf(sb, "%s{\"id\":\"%s\",\"name\":", first ? "" : ",", it);
      sb_json_str(sb, name);
      sb_printf(sb, ",\"recent\":%lld}", recent);
      first = 0;
    }
    sqlite3_finalize(st);
  }
  sb_puts(sb, "]}");
}

int folders_list_json(hh_t *h, strbuf_t *sb) {
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folders ORDER BY created_at, title_id");
  if (!st) return -1;
  char ids[FOLDER_MAX][16];
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_MAX)
    snprintf(ids[n++], 16, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  sb_puts(sb, "[");
  for (int i = 0; i < n; i++) {
    if (i) sb_puts(sb, ",");
    folder_json(h, ids[i], sb);
  }
  sb_puts(sb, "]");
  return 0;
}

int folders_get_json(hh_t *h, const char *tid, strbuf_t *sb) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  folder_json(h, tid, sb);
  return 0;
}

int folders_of_title(hh_t *h, const char *tid, char *out, size_t outsz) {
  out[0] = '\0';
  sqlite3_stmt *st = prep(h, "SELECT folder FROM hh.folder_items WHERE title_id=?1");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) snprintf(out, outsz, "%.15s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* save / delete                                                             */
/* ------------------------------------------------------------------------- */

static int clean_name(const char *in, char *out, size_t outsz) {
  while (*in == ' ') in++;
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < outsz; p++) {
    if (*p < 0x20 || *p == 0x7f) continue;
    out[n++] = (char)*p;
  }
  while (n && out[n - 1] == ' ') n--;
  /* never cut a UTF-8 sequence in half */
  while (n && ((unsigned char)out[n - 1] & 0xC0) == 0x80 && n == outsz - 1) n--;
  out[n] = '\0';
  return n > 0 && n <= FOLDER_NAME_MAX ? 0 : -1;
}

int folders_save(hh_t *h, const char *tid_in, const char *name_in, const char *tab,
                 const char *color, const char *items_csv, char *out_tid,
                 char *note, size_t notesz) {
  if (note) note[0] = '\0';
  hh_log("folder save: id='%s' tab=%s color=%s items='%.200s'", tid_in ? tid_in : "", tab ? tab : "",
         color ? color : "", items_csv ? items_csv : "");
  if (h->nusers == 0) {
    ferr(h, "The home screen database layout isn't supported.%s", "");
    return -1;
  }
  char name[FOLDER_NAME_MAX + 8];
  if (!name_in || clean_name(name_in, name, sizeof(name))) {
    ferr(h, "Give the folder a name (up to %s characters).", "48");
    return -1;
  }
  if (!tab || (strcmp(tab, "games") && strcmp(tab, "media"))) tab = "games";
  if (!icon_valid_color(color)) color = "blue";

  /* parse and check the games */
  char (*items)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  if (!items) return -1;
  int n = 0;
  const char *p = items_csv ? items_csv : "";
  while (*p && n < FOLDER_ITEMS_MAX) {
    size_t len = strcspn(p, ",");
    char t[32];
    if (len > 0 && len < sizeof(t)) {
      memcpy(t, p, len);
      t[len] = '\0';
      int dup = 0;
      for (int i = 0; i < n; i++) dup |= !strcmp(items[i], t);
      if (!dup && hh_valid_tid(t) && !folders_is_folder(t) && !hh_is_locked(t) && !hh_is_internal(t))
        snprintf(items[n++], 16, "%.15s", t);
    }
    p += len;
    if (*p == ',') p++;
  }

  /* folder id */
  char tid[16] = "";
  if (tid_in && tid_in[0]) {
    if (!folders_is_folder(tid_in) ||
        count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid_in) == 0) {
      free(items);
      ferr(h, "That folder doesn't exist.%s", "");
      return -1;
    }
    snprintf(tid, sizeof(tid), "%s", tid_in);
  } else {
    for (int i = 1; i <= FOLDER_MAX && !tid[0]; i++) {
      char cand[16];
      snprintf(cand, sizeof(cand), FOLDER_PREFIX "%05d", i);
      if (count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", cand) == 0 &&
          !tile_present(h, cand))
        snprintf(tid, sizeof(tid), "%s", cand);
    }
    if (!tid[0]) {
      free(items);
      ferr(h, "You already have the maximum of %s folders.", "99");
      return -1;
    }
  }

  /* 1. records (one transaction) */
  char (*removed)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  int *removed_hidden = calloc(FOLDER_ITEMS_MAX, sizeof(int));
  char (*touched)[16] = calloc(FOLDER_MAX, 16); /* other folders that lost games */
  int nrem = 0, ntouch = 0;
  if (!removed || !removed_hidden || !touched) goto oom;
  if (run1(h, "BEGIN IMMEDIATE", NULL, NULL)) goto fail;
  {
    sqlite3_stmt *st = prep(h,
        "INSERT INTO hh.folders(title_id, name, tab, color, custom_icon, created_at) "
        "VALUES(?1, ?2, ?3, ?4, 0, datetime('now')) ON CONFLICT(title_id) DO UPDATE SET "
        "name=excluded.name, tab=excluded.tab, color=excluded.color");
    if (!st) goto rollback;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tab, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, color, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
      goto rollback;
    }

    /* games that left this folder */
    st = prep(h, "SELECT title_id, was_hidden FROM hh.folder_items WHERE folder=?1");
    if (!st) goto rollback;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && nrem < FOLDER_ITEMS_MAX) {
      const char *it = (const char *)sqlite3_column_text(st, 0);
      int keep = 0;
      for (int i = 0; i < n && !keep; i++) keep = !strcmp(items[i], it);
      if (keep) continue;
      snprintf(removed[nrem], 16, "%s", it);
      removed_hidden[nrem++] = sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < nrem; i++)
      if (run1(h, "DELETE FROM hh.folder_items WHERE title_id=?1", removed[i], NULL)) goto rollback;

    /* games in the folder now, in order */
    for (int i = 0; i < n; i++) {
      char other[16] = "";
      folders_of_title(h, items[i], other, sizeof(other));
      if (other[0] && strcmp(other, tid)) {
        int seen = 0;
        for (int k = 0; k < ntouch; k++) seen |= !strcmp(touched[k], other);
        if (!seen && ntouch < FOLDER_MAX) snprintf(touched[ntouch++], 16, "%s", other);
      }
      st = prep(h,
          "INSERT INTO hh.folder_items(title_id, folder, pos, was_hidden) VALUES(?1, ?2, ?3, "
          " (SELECT count(*) FROM hh.wanted WHERE title_id=?1)) "
          "ON CONFLICT(title_id) DO UPDATE SET folder=excluded.folder, pos=excluded.pos");
      if (!st) goto rollback;
      sqlite3_bind_text(st, 1, items[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, tid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(st, 3, i);
      rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE) {
        ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
        goto rollback;
      }
    }
  }
  if (run1(h, "COMMIT", NULL, NULL)) goto rollback;

  /* 2. hide the games inside, show the ones that left */
  int missing = 0;
  for (int i = 0; i < n; i++)
    if (hh_hide(h, items[i])) missing++;
  for (int i = 0; i < nrem; i++)
    if (!removed_hidden[i]) hh_unhide(h, removed[i]);

  /* 3. the tile itself (+ refresh folders that lost games to this one) */
  int rc = install_tile(h, tid, note, notesz);
  for (int k = 0; k < ntouch; k++) install_tile(h, touched[k], NULL, 0);
  if (rc) goto fail;
  hh_log("folder %s: saved, %d game(s), %d hide failure(s), %d removed", tid, n, missing, nrem);
  if (missing && note && !note[0])
    snprintf(note, notesz, "%d game(s) couldn't be hidden from the home screen.", missing);

  snprintf(out_tid, 16, "%s", tid);
  free(items);
  free(removed);
  free(removed_hidden);
  free(touched);
  return 0;

rollback:
  run1(h, "ROLLBACK", NULL, NULL);
  goto fail;
oom:
  ferr(h, "Out of memory.%s", "");
fail:
  hh_log("folder save failed: %s", h->err);
  free(items);
  free(removed);
  free(removed_hidden);
  free(touched);
  return -1;
}

static void remove_tile_files(const char *tid) {
  char p[500];
  snprintf(p, sizeof(p), "%s/%s/sce_sys/param.json", F.app_dir, tid);
  unlink(p);
  snprintf(p, sizeof(p), "%s/%s/sce_sys/icon0.png", F.app_dir, tid);
  unlink(p);
  snprintf(p, sizeof(p), "%s/%s/sce_sys/pic0.dds", F.app_dir, tid);
  unlink(p);
  snprintf(p, sizeof(p), "%s/%s/sce_sys", F.app_dir, tid);
  rmdir(p);
  snprintf(p, sizeof(p), "%s/%s", F.app_dir, tid);
  rmdir(p);
}

int folders_delete(hh_t *h, const char *tid) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  /* show the games again (unless they were hidden before joining) */
  char (*show)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  int n = 0;
  if (!show) return -1;
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 AND was_hidden=0");
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_ITEMS_MAX)
      snprintf(show[n++], 16, "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  if (run1(h, "DELETE FROM hh.folder_items WHERE folder=?1", tid, NULL) ||
      run1(h, "DELETE FROM hh.folders WHERE title_id=?1", tid, NULL)) {
    free(show);
    return -1;
  }
  for (int i = 0; i < n; i++) hh_unhide(h, show[i]);
  free(show);

  int un = plat_app_uninstall(tid);
  hh_log("folder %s: deleted (uninstall 0x%08X, %d game(s) shown)", tid, (unsigned)un, n);
  remove_tile_files(tid);
  char cpath[400];
  custom_icon_path(tid, cpath, sizeof(cpath));
  unlink(cpath);
  bg_dds_path(tid, cpath, sizeof(cpath));
  unlink(cpath);
  bg_jpg_path(tid, cpath, sizeof(cpath));
  unlink(cpath);
  return 0;
}

int folders_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  char cpath[400], dir[400];
  custom_icon_path(tid, cpath, sizeof(cpath));
  if (img) {
    unsigned char *png;
    size_t pl;
    if (icon_from_image(img, len, NULL, &png, &pl)) {
      ferr(h, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).%s", "");
      return -1;
    }
    snprintf(dir, sizeof(dir), "%s/folders", F.data_dir);
    mkdirs(dir);
    int rc = write_file(cpath, png, pl);
    free(png);
    if (rc) {
      ferr(h, "Couldn't save the picture.%s", "");
      return -1;
    }
  } else {
    unlink(cpath);
  }
  if (run1(h, "UPDATE hh.folders SET custom_icon=?2 WHERE title_id=?1", tid, img ? "1" : "0"))
    return -1;
  return install_tile(h, tid, NULL, 0);
}

int folders_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  char dds_path[400], jpg_path[400], dir[400];
  bg_dds_path(tid, dds_path, sizeof(dds_path));
  bg_jpg_path(tid, jpg_path, sizeof(jpg_path));
  if (img) {
    /* a 4K BC7 texture, like the ones the console makes for its own apps */
    tex_fmt_t f = {TEX_DDS, TEX_BG_W, TEX_BG_H, 1, 98, 0};
    unsigned char *dds = NULL, *jpg = NULL;
    size_t dl = 0, jl = 0;
    if (tex_encode(img, len, &f, &dds, &dl)) {
      ferr(h, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).%s", "");
      return -1;
    }
    uint8_t *rgba = icon_load_cover(img, len, TEX_BG_W, TEX_BG_H);
    if (rgba) img_encode_jpg(rgba, TEX_BG_W, TEX_BG_H, 90, &jpg, &jl);
    free(rgba);
    snprintf(dir, sizeof(dir), "%s/folders", F.data_dir);
    mkdirs(dir);
    int rc = write_file(dds_path, dds, dl) || (jpg && write_file(jpg_path, jpg, jl));
    free(dds);
    free(jpg);
    if (rc) {
      ferr(h, "Couldn't save the background.%s", "");
      return -1;
    }
    hh_log("folder %s: background set (%zu bytes)", tid, dl);
  } else {
    unlink(dds_path);
    unlink(jpg_path);
    hh_log("folder %s: background removed", tid);
  }
  return install_tile(h, tid, NULL, 0);
}

int folders_reapply(hh_t *h, int *fixed) {
  *fixed = 0;
  if (h->nusers == 0) return 0;
  sqlite3_stmt *st = prep(h, "SELECT title_id, tab FROM hh.folders");
  if (!st) return -1;
  char ids[FOLDER_MAX][16], tabs[FOLDER_MAX][8];
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_MAX) {
    snprintf(ids[n], 16, "%s", sqlite3_column_text(st, 0));
    snprintf(tabs[n], 8, "%s", sqlite3_column_text(st, 1));
    n++;
  }
  sqlite3_finalize(st);
  for (int i = 0; i < n; i++) {
    char p[500];
    struct stat sb;
    snprintf(p, sizeof(p), "%s/%s/sce_sys/param.json", F.app_dir, ids[i]);
    if (!tile_present(h, ids[i]) || stat(p, &sb) != 0) {
      if (install_tile(h, ids[i], NULL, 0) == 0) (*fixed)++;
    } else if (place_tile(h, ids[i], tabs[i])) {
      (*fixed)++;
    }
  }
  return 0;
}

void folders_remove_all_tiles(hh_t *h) {
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folders");
  if (!st) return;
  char ids[FOLDER_MAX][16];
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_MAX)
    snprintf(ids[n++], 16, "%s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  for (int i = 0; i < n; i++) {
    plat_app_uninstall(ids[i]);
    remove_tile_files(ids[i]);
  }
}
