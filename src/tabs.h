/* Moving a tile between the Games and Media tabs.
 *
 * Which tab a tile sits on is the Games (0x02) / Media (0x04) bits of
 * tbl_iconinfo_<u>.dispLocation. Home Hider sets the whole value to the
 * one the console uses for that tab, so a moved tile looks like a native
 * one there. The original dispLocation is saved in state.db, so the move
 * can be undone exactly, and it is re-applied at each launch (and in the
 * background) in case the system put it back.
 *
 * Note: this only changes which tab a tile appears on. It does not change
 * how the console launches it. A web-app / autoloader tile moved to the
 * Games tab may show only its logo, because the console tries to start
 * Games-tab tiles like a game; put those in a folder instead.
 */
#pragma once

#include "hider.h"
#include "strbuf.h"

/* Moves tid to "games" or "media". 0 on success. */
int  tabs_move(hh_t *h, const char *tid, const char *dest);
/* Puts tid back on the tab it was on before Home Hider moved it. */
int  tabs_reset(hh_t *h, const char *tid);
/* Re-applies every saved move. *fixed = how many were put back. */
int  tabs_apply(hh_t *h, int *fixed);
/* JSON: [{"id","dest"}...] of tiles Home Hider has moved. */
int  tabs_moved_json(hh_t *h, strbuf_t *sb);
