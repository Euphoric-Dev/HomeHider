/* Renaming tiles and giving them a different picture.
 *
 * The home screen takes a tile's name and picture from app.db:
 *   - name:    titleName / conceptName cells of the rows that belong to the
 *              title (tbl_iconinfo_<u>, tbl_concepticoninfo_<u>,
 *              tbl_contentinfo, ... found by titleId or concept id)
 *   - picture: the icon0Info path in those rows
 *   - background: the pic0Info / pic1Info paths (see texture.h for formats)
 * Home Hider writes the new picture to <appmeta>/<title id>/hh_icon0_<n>.png
 * (a new file name each time, so the home screen can't show a cached copy),
 * points icon0Info at it and changes the name cells. Every original value is
 * saved in state.db, so "Use the original" puts back exactly what was there.
 * Nothing in the game's own files is changed.
 */
#pragma once

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

#define CUSTOM_NAME_MAX 64

typedef struct {
  const char *data_dir;     /* our copy of each picture: <data_dir>/icons */
  const char *appmeta_dir;  /* "/user/appmeta" */
} custom_cfg_t;

void custom_init(const custom_cfg_t *cfg);

/* name NULL or "" (or the original name) puts the original name back. */
int  custom_set_name(hh_t *h, const char *tid, const char *name);
/* img NULL puts the original picture back. clear = keep see-through parts
 * transparent instead of putting them on a dark background. */
int  custom_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len, int clear);
/* Home screen background (pic0/pic1). img NULL puts the original back. */
int  custom_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len);
/* The chosen background picture (a JPEG copy), for previews. 0 if any. */
int  custom_bg_path(hh_t *h, const char *tid, char *out, size_t n);

/* {"id","name","orig_name","renamed","custom_icon","custom_bg","can_rename",
 *  "can_icon","can_bg"} */
int  custom_get_json(hh_t *h, const char *tid, strbuf_t *sb);

/* Launch-time repair: puts custom names/pictures back if the system
 * restored the originals. *fixed = number of values changed. */
int  custom_reapply(hh_t *h, int *fixed);

/* Deletes Home Hider's picture files (before the original database is
 * restored, which brings back the original names and pictures). */
void custom_forget_all(hh_t *h);
