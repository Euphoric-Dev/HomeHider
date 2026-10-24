/* Starting games and apps from the menu and from folder pages.
 *
 * - Web link tiles (Payload Manager, Web File Explorer, ...) have no program
 *   to start: their param.json holds a deeplinkUri the home screen opens in
 *   the browser. Home Hider opens that address instead of asking the
 *   launcher, which can only answer "can't open".
 * - Everything else is started on a worker thread, so the menu gets an answer
 *   right away and polls for the result (the browser may be paused while the
 *   game takes over the screen).
 * - "Another game is running" is reported so the page can offer to close it,
 *   like the home screen does. "Already running" counts as started.
 */
#pragma once

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

typedef struct {
  const char *app_dir;      /* "/user/app" */
  const char *appmeta_dir;  /* "/user/appmeta" */
} launch_cfg_t;

void launch_init(const launch_cfg_t *cfg);

/* 0 and url filled if tid is a web link tile (http/https deeplink, no
 * program of its own). */
int  launch_web_url(hh_t *h, const char *tid, char *url, size_t n);

/* Starts a launch job. name = title shown in messages; close_running =
 * close the game that is running first (after the user said OK). Returns
 * the job id (> 0), or -1 if the previous job is still running. */
int  launch_start(const char *tid, const char *name, int close_running);

/* JSON for /api/launch_status. name_of looks up a title's name (called on
 * the caller's thread, for the "another game is running" answer). */
void launch_status_json(int job, strbuf_t *sb,
                        void (*name_of)(const char *tid, char *out, size_t n));

/* Human-readable meaning of a launcher error code. */
const char *launch_error_text(int rc);
