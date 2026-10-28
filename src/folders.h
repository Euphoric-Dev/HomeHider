/* Folders: one home screen tile that opens a page listing several games.
 *
 * A folder is a small launcher app (like Payload Manager's tile) installed at
 * <app_dir>/HHFDnnnnn with a param.json whose deeplink opens
 * http://127.0.0.1:<port>/folder/HHFDnnnnn in the PS5 browser. The games put
 * in a folder are hidden from the home screen with the normal hide mechanism,
 * and shown again when they leave the folder (unless they were hidden before).
 */
#pragma once

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

#define FOLDER_PREFIX   "HHFD"
#define FOLDER_MAX      99
#define FOLDER_ITEMS_MAX 300
#define FOLDER_NAME_MAX 48

typedef struct {
  const char *data_dir;  /* custom icons live in <data_dir>/folders */
  const char *app_dir;   /* "/user/app" */
  int port;              /* menu port, for the tile's deeplink */
} folders_cfg_t;

void folders_init(const folders_cfg_t *cfg);
int  folders_is_folder(const char *tid);

/* JSON array of folders with their items. */
int  folders_list_json(hh_t *h, strbuf_t *sb);
/* JSON object for one folder; -1 if it doesn't exist. */
int  folders_get_json(hh_t *h, const char *tid, strbuf_t *sb);

/* Creates (tid empty) or updates a folder. items_csv = "PPSA01325,CUSA00900".
 * out_tid receives the folder id. note gets a warning, if any. 0 on success. */
int  folders_save(hh_t *h, const char *tid, const char *name, const char *tab,
                  const char *color, const char *items_csv, char *out_tid,
                  char *note, size_t notesz);
int  folders_delete(hh_t *h, const char *tid);
/* Moves a folder's tile to the Games or Media tab. */
int  folders_set_tab(hh_t *h, const char *tid, const char *tab);
/* Takes one app out of its folder, leaving it on its tab. */
int  folders_remove_member(hh_t *h, const char *tid);
/* img == NULL goes back to the automatic collage. */
int  folders_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len);
/* Home screen background for the folder tile (sce_sys/pic0.dds). img NULL
 * removes it. */
int  folders_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len);
/* The chosen background picture (JPEG), for previews. 0 if there is one. */
int  folders_bg_path(const char *tid, char *out, size_t outsz);
/* Path of the tile's current icon0.png. */
int  folders_icon_path(const char *tid, char *out, size_t outsz);

/* Launch-time repair: reinstall missing tiles, put tiles back on their tab.
 * *fixed = number of folders that needed it. */
int  folders_reapply(hh_t *h, int *fixed);
/* Uninstalls every folder tile (used before restoring the original database). */
void folders_remove_all_tiles(hh_t *h);
/* Folder id holding tid ("" if none). */
int  folders_of_title(hh_t *h, const char *tid, char *out, size_t outsz);
