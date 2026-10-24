/* Live wallpapers: LiveShell Engine by ZiZc3 (GPL-3.0), carried inside
 * Home Hider. https://github.com/ZiZc3/LiveShell
 *
 * Home Hider holds an unchanged copy of liveshell.elf (third_party/liveshell)
 * and hands it to the ELF loader, exactly as if it had been loaded from
 * Payload Manager. LiveShell then runs on its own and serves its API on
 * 127.0.0.1:9050; Home Hider's menu talks to that API for the basics (on/off,
 * pick and delete videos) and links to LiveShell's own page for the rest.
 */
#pragma once

#include <stddef.h>

#include "strbuf.h"

#define LIVESHELL_VERSION "1.5.2"

typedef struct {
  int loader_port;  /* ELF loader, 9021 */
  int api_port;     /* LiveShell's web API, 9050 */
} live_cfg_t;

void live_init(const live_cfg_t *cfg);

/* 1 if LiveShell answers on its port. */
int  live_running(void);

/* Hands the built-in liveshell.elf to the ELF loader. 0 on success. */
int  live_start(void);

/* Asks LiveShell to stop (the home screen restarts clean). 0 if it answered. */
int  live_stop(void);

/* GET path (with query) on LiveShell's API. Returns the HTTP status, or -1
 * if LiveShell isn't running. *body is malloc'd (NUL-terminated). */
int  live_get(const char *path, char **body, size_t *len);

/* Index of the video with this path in LiveShell's list, -1 if none. */
int  live_video_index(const char *path);

/* Size and SHA-256 of the built-in liveshell.elf, for Diagnostics. */
size_t live_elf_size(void);
void   live_elf_sha256(char hex[65]);
