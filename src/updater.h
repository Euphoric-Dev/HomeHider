/* Home Hider self-updater.
 *
 * Sources: the latest GitHub release of a repository, a USB drive, or a file
 * uploaded from the menu. Every candidate must be an x86-64 ELF carrying the
 * Home Hider version marker; GitHub downloads are checked against the
 * release asset's SHA-256 digest when GitHub provides one.
 *
 * Installing replaces every Home Hider copy found in Payload Manager's
 * payload folder (old copy kept in the backup folder), then starts the new
 * version through the ELF loader; the new instance asks this one to exit.
 */
#pragma once

#include <stddef.h>

#include "strbuf.h"

typedef struct {
  const char *data_dir;     /* /data/homehider */
  const char *backup_dir;   /* /data/homehider/backup */
  const char *pldmgr_dir;   /* /data/pldmgr/payloads */
  const char *usb_fmt;      /* "/mnt/usb%d" */
  const char *github_api;   /* "https://api.github.com" */
  int         loader_port;  /* ELF loader, 9021 */
} upd_config_t;

#define UPD_MAX_ELF (32u * 1024 * 1024)

/* The marker embedded in this build ("HOMEHIDER-ELF-VERSION=x.y.z;"). */
const char *upd_marker(void);

void upd_init(const upd_config_t *cfg);
void upd_set_repo(const char *repo);            /* "owner/name" or "" */
int  upd_valid_repo(const char *repo);

/* Starts a background check. Returns 0 if started, -1 if busy. */
int  upd_start_check(int github, int usb, int notify_if_found);
/* Starts a background install from "github" or "usb". 0 if started. */
int  upd_start_install(const char *source);
/* Installs an uploaded ELF right away. Returns 0 on success; msg explains. */
int  upd_install_upload(const unsigned char *buf, size_t len, int allow_older,
                        char *msg, size_t msgsz);

void upd_status_json(strbuf_t *sb);

/* Helpers exposed for tests/diagnostics. */
int  upd_version_cmp(const char *a, const char *b);
int  upd_find_version(const unsigned char *buf, size_t len, char *ver, size_t versz);
