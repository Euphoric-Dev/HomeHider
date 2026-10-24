/* Console-specific services. platform_ps5.c is used on the PS5,
 * platform_host.c for desktop testing. */
#pragma once

#include <stddef.h>
#include <stdint.h>

void plat_init(void);
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Opens the PS5 web browser at url. Returns 0 on success. */
int  plat_launch_browser(const char *url);
/* Writes the console's LAN IPv4 address. Returns 0 on success. */
int  plat_get_ip(char *buf, size_t n);
/* Launcher (sceLncUtil) error codes Home Hider handles itself. */
#define LNC_ERR_NOT_INITIALIZED   0x80940001
#define LNC_ERR_ALREADY_RUNNING   0x8094000c
#define LNC_ERR_KILL_NEEDED       0x80940010 /* another game must close first */
#define LNC_ERR_SUSPEND_NEEDED    0x80940011
#define LNC_ERR_APP_NOT_FOUND     0x80940031

/* Starts a game/app by title id for the signed-in user. Returns 0 on success
 * (*app_id = the running app's id), otherwise the system error code. */
int  plat_launch_app(const char *title_id, uint32_t *app_id);
/* App id of a running title. 0 if it is running. */
int  plat_app_id(const char *title_id, uint32_t *app_id);
/* The game (big app) running right now. 0 if there is one. */
int  plat_running_game(char *title_id, size_t n, uint32_t *app_id);
/* Closes a running app. Writes what was tried into report. 0 on success. */
int  plat_kill_app(uint32_t app_id, char *report, size_t n);
/* GETs url (https on the console), following redirects. On HTTP 200, *out
 * holds a malloc'd body of *out_len bytes (at most max). Returns the HTTP
 * status, or a negative value on a network/TLS failure or oversize body.
 * *progress (optional) is updated with bytes received. */
int  plat_http_get(const char *url, unsigned char **out, size_t *out_len,
                   size_t max, volatile long long *progress);
/* Registers the app folder app_dir/<title_id> (sce_sys/param.json +
 * icon0.png) as a home screen tile, or refreshes it. 0 on success. */
int  plat_app_install(const char *title_id, const char *app_dir);
/* Removes a tile installed with plat_app_install. 0 on success. */
int  plat_app_uninstall(const char *title_id);
/* Something that changes every time the console starts (the boot time).
 * 0 on success. */
int  plat_boot_id(char *out, size_t n);
/* Restarts the home screen process (SceShellUI). Returns 0 if it was signalled. */
int  plat_reload_shellui(void);
