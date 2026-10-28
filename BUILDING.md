# Building Home Hider

## Requirements

- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) installed at `/opt/ps5-payload-sdk`. Its release zip unpacks there.
- clang and lld 18, make, python3

## Build

```sh
make                                     # -> HomeHider.elf
make HH_GITHUB_REPO=yourname/HomeHider   # same, with your repo as the default update source
make test                                # desktop test suite (no PS5 needed)
make deploy PS5_HOST=192.168.1.50        # send to the ELF loader on port 9021
```

`make test` builds a desktop copy of Home Hider with AddressSanitizer, then drives it over HTTP. It covers:

- a mock `app.db`
- a fake GitHub API
- a fake USB drive, with pictures in nested folders and EXIF-rotated photos (made with Pillow when it's installed)
- a fake ELF loader

## Publishing a release (so the updater finds it)

1. Raise `HH_VERSION` in `src/hider.h`, for example `0.3.1`. The updater compares this number, so every release needs a higher one.
2. Run `make`. The build embeds the marker `HOMEHIDER-ELF-VERSION=<version>;` in the ELF. The updater uses it to recognise Home Hider builds.
3. On GitHub, create a release tagged `v<version>`, for example `v0.3.1`.
4. Attach `HomeHider.elf` to the release. The updater picks the `.elf` asset, preferring names that contain "homehider".
5. Publish the release. Drafts and pre-releases are ignored.

GitHub adds a SHA-256 digest to each uploaded asset, and Home Hider refuses a download that doesn't match it.

## How the updater installs

1. Downloads the release asset over HTTPS with the system HTTP stack (`libSceHttp2`), or reads it from USB, or takes it from a menu upload.
2. Validates it: an x86-64 ELF, 16 KB to 32 MB, carrying the Home Hider version marker, and newer than the running version unless "Allow older versions" is ticked.
3. Finds every Home Hider copy under `/data/pldmgr/payloads`. A copy counts if it has the version marker or the `/data/homehider` path string, which v0.1 and v0.2 have. Each copy is backed up to `/data/homehider/backup/HomeHider-<old version>.elf`, then replaced atomically.
4. Writes `/data/homehider/.updated` and sends the new build to the ELF loader on port 9021. The new instance asks the old one to quit, takes over port 8090, and skips reopening the browser.

## Source layout

```
src/main.c          startup, HTTP routes, settings, backups
src/hider.c/.h      app.db logic: list, hide/show, re-apply, locked tiles, diagnostics
src/updater.c/.h    update checks (GitHub/USB), validation, install, restart
src/folders.c/.h    folders: records, tile install (param.json + icon), tab placement
src/icons.c/.h      folder artwork (collage / custom picture) via stb_image; EXIF rotation, transparency
src/usbpics.c/.h    picture browser for USB drives and /data/homehider/pictures, cached previews
src/launch.c/.h     starting titles: web link tiles, launch jobs, "another game running"
src/custom.c/.h     renaming tiles, changing their pictures and backgrounds (app.db cells + appmeta files)
src/texture.c/.h    background files: PNG/JPEG, DDS with BC7 (mode 6), BC1, BC3, RGBA8/BGRA8 and mips
src/liveshell.c/.h  live wallpaper: starts the built-in LiveShell Engine, talks to its API on port 9050
src/arrange.c/.h    pre-arrange the home screen: order via lastAccessIndex, pin via MyHome
third_party/liveshell  LiveShell Engine 1.5.2 by ZiZc3 (GPL-3.0): liveshell.elf + its source, see NOTICE.md
src/log.c/.h        log file shown in Diagnostics
src/json.c/.h       small JSON reader for GitHub API replies
src/sha256.c/.h     SHA-256 for download verification
src/http.c/.h       minimal HTTP server helpers
src/platform_ps5.c  notifications, browser/app launch, boot time, HTTPS, IP, ShellUI restart
src/platform_host.c desktop stand-ins used by the tests
web/index.html      the menu (embedded into the ELF at build time)
web/folder.html     the page a folder tile opens
tests/test_host.py  end-to-end tests
```

## How hiding works

The home screen reads its layout from `/system_data/priv/mms/app.db` (SQLite). For each console user `<u>`, Home Hider makes three changes:

1. sets `tbl_iconinfo_<u>.visible` to `0`
2. clears the Games (`0x02`) and Media (`0x04`) bits in `tbl_iconinfo_<u>.dispLocation`
3. removes the tile's `MyHome` row from `tbl_group_<u>`, after backing it up

Original values are saved in `/data/homehider/state.db`, so **Show** restores exactly what was there.

Game Library (`NPXS40071`) and App Library (`NPXS40139`) are added by the home screen code itself. On FW 4.51, database changes don't affect them, so Home Hider doesn't offer them. "All Apps" (`NPXS40056`) hides normally.

## How folders work

A folder is a small launcher app, the same kind Payload Manager installs for itself:

- It lives at `/user/app/HHFDnnnnn/sce_sys/`, with a `param.json` whose `deeplinkUri` is `http://127.0.0.1:8090/folder/HHFDnnnnn`, plus a 512x512 `icon0.png`.
- Home Hider registers the app with `sceAppInstUtilAppInstallTitleDir`, resolved at run time like Payload Manager does. Then it sets the tile's `dispLocation` tab bits for the chosen tab.
- The games inside are hidden with the normal hide mechanism. `folder_items.was_hidden` remembers games that were already hidden, so taking a game out of a folder never shows something the user hid on purpose.
- At every launch, missing folder tiles are reinstalled and moved tiles are put back on their tab.

### Folder pictures

The PS5 browser can't open files, so the editor asks Home Hider for pictures instead. `/api/usb_images` lists PNG, JPG, BMP, GIF and TGA files on `/mnt/usb0`–`/mnt/usb7` (up to four folders deep, which reaches `PS5/CREATE/Screenshots/<game>/`) and in `/data/homehider/pictures`. Previews come from `/api/usb_thumb`: 192x192 PNGs cached in `/data/homehider/thumbs`. Only paths under those roots with a picture extension and no `..` are read. The chosen picture is decoded with stb_image, turned upright from its EXIF orientation, centre-cropped to 512x512 and saved with the folder, so the USB drive can be removed afterwards. A folder keeps its picture with transparency and puts it on the folder's colour each time it writes `icon0.png`, so a colour change repaints the background (see Transparency below).

### Starting titles

`/api/launch` answers at once and the page polls `/api/launch_status`, because the PS5 browser can pause while a game takes the screen.

- **Web link tiles.** If a title has no `eboot.bin` and its `param.json` (in `/user/app/<id>/sce_sys/`, `/user/appmeta/<id>/`, or `tbl_contentinfo.AppInfoJson`) has an `http(s)` `deeplinkUri`, the launcher can't start it, which is the "can't open" error. Home Hider returns the address instead. The page opens it, or Home Hider opens the PS5 browser when the menu is used from a phone or PC.
- **Results.** `sceLncUtilLaunchApp` returns the new app id (`0x6xxxxxxx`) on success, and an error with the top bit set. `0x8094000C` (already running) counts as started. `0x80940010` / `0x80940011` (another app must close or suspend) is reported with the running game, found with `sceLncUtilGetAppIdOfRunningBigApp` and `sceLncUtilGetAppTitleId`, so the page can ask. With `close_running=1`, that game is closed with `sceLncUtilKillApp` (falling back to `sceSystemServiceKillApp(id, -1, 0, 0)`), and Home Hider waits until it's gone before it starts the new one. Other codes are turned into a sentence, and the code is logged.
- **Run-time lookup.** Launcher functions other than `sceLncUtilLaunchApp` and `sceLncUtilKillApp` are looked up with `kernel_dynlib_dlsym` in `libSceSystemService.sprx`. A missing export then only disables that feature, instead of stopping the payload from loading. `sceLncUtilInitialize` is called at start.

### Backgrounds

The home screen draws a tile's background from the `pic0Info` / `pic1Info` paths in `app.db`, found the same way as `icon0Info`. For each cell, Home Hider:

1. Takes the original path (for cells already pointing at Home Hider's files, the saved original) and reads the original file's header, when it can: DDS size, mip count and texture format (DX10 header or legacy `DXT1`/`DXT5`/RGBA masks), or PNG/JPEG size. Originals inside game packages it can't read are treated as 3840x2160 BC7 DDS without mips.
2. Writes one file per distinct format to `/user/appmeta/<id>/hh_bg_<time>_<n>.<ext>` (a new name each time, so the picture cache can't show an old one). DDS textures are encoded in the same format with the same mips: BC7 (mode 6 with a least-squares refinement), BC1, BC3, or uncompressed RGBA8/BGRA8. Anything else is refused and logged.
3. Points the cells at the new files in one short transaction, after the files exist, and saves the originals in `custom_cells` (kind `bg`).

The chosen picture is kept as a 3840x2160 JPEG in `/data/homehider/backgrounds/`. At start, if a cell was reset or a file is missing, the files are made again from it. Folder tiles get a 3840x2160 BC7 `sce_sys/pic0.dds`, which is kept in `/data/homehider/folders/<id>_pic0.dds` and written with every tile install.

`tests/test_host.py` decodes the generated DDS files with Pillow to check them.

### Transparency

Pictures are handled as RGBA throughout. Folder colour `none` keeps the collage or custom picture see-through (RGBA `icon0.png`). With a colour, the see-through parts are put on the colour's gradient when `icon0.png` is written. Tile pictures get a dark background unless `clear=1` is sent.

### Arranging the home screen

`arrange.c` forces a tile order the PS5 has no stock UI for. The home row sorts by `tbl_iconinfo_<u>.lastAccessIndex` (counted by `tbl_version.access_index`). Home Hider writes arranged tiles' `lastAccessIndex` to `base - pos`, where `base` is the console's counter plus a large gap, so they sit in front in the chosen order and normal play (which gets the next, smaller index) can't catch up. "Pin" also adds the tile to the per-user `MyHome` group (`tbl_group_<u>`). The true original of every `lastAccessIndex` and pin state is saved once in state.db (`arrange_orig`), so clearing restores exactly and a pin the console already had is never removed. It is re-applied at launch like hidden tiles. API: `/api/arrange` (GET), `/api/arrange/save` (items + pinned), `/api/arrange/clear`.

### Live wallpaper (LiveShell Engine)

[LiveShell Engine](https://github.com/ZiZc3/LiveShell) by ZiZc3 is built in unchanged. `make` turns `third_party/liveshell/liveshell.elf` into `src/liveshell_elf.h`, a byte array inside `HomeHider.elf`. With the `live` setting on, Home Hider sends those bytes to the ELF loader (port 9021), 3 seconds after it starts, unless LiveShell already answers on port 9050. That's the same as loading `liveshell.elf` from Payload Manager.

The menu's Live wallpaper section goes through Home Hider (`/api/live`, `/api/live/power`, `/api/live/select`, `/api/live/delete`, `/api/live/thumb`). Home Hider forwards these to LiveShell's API on `127.0.0.1:9050`, so the menu works from a phone or PC too, and the POST requests get Home Hider's cross-site check. Videos are picked by path, and Home Hider turns the path into LiveShell's list index just before sending. Turning the feature off calls LiveShell's `/api/shutdown`. Everything else (looks, playlists, per-game videos, Discover, uploads) stays on LiveShell's own page. The tests use a fake LiveShell and a recording ELF loader, and check that the exact `liveshell.elf` bytes are sent. See `third_party/liveshell/NOTICE.md` for updating to a newer LiveShell.

### Menu at start

**Options > When Home Hider starts** sets `open_browser`: `1` always opens the menu, `0` never does, and `2` is "quiet when started by Autoload". In mode `2`, Home Hider stays quiet on the first start after the console boots. It tells boots apart by `kern.boottime`, which it saves in `/data/homehider/last_boot.txt`. A later start in the same session opens the menu: launching again from Payload Manager also stops the copy that is already running, and either of those means a person started it. The decision is logged as `startup: menu mode ...`.

### Names and pictures

`custom.c` finds a title's rows in every `app.db` table, by `titleId`, or by `localConceptId`/`conceptId` for concept tables. A concept id shared by more than three tiles is treated as a placeholder and skipped. In those rows:

- **Names:** columns whose name contains "name" and that hold one of the title's names (a `titleName`/`conceptName` value, a saved original or the previous custom name) are set to the new name.
- **Pictures:** `icon0Info` is pointed at `/user/appmeta/<id>/hh_icon0_<time>.png`, a 512x512 PNG. The file name changes each time so the home screen's picture cache can't show the old one.

Each original value goes to `state.db` (`custom_cells`) before it changes, and putting an original back only touches cells that still hold Home Hider's value. At start, `custom_reapply` re-points cells the console reset and rewrites a missing picture file from `/data/homehider/icons/`.

The grey bar at the top of the folder page belongs to the PS5 browser. A web page can't remove it. The page asks for full screen on the first button press, which the browser may or may not allow.

License: GPL-3.0 (see `LICENSE`), because Home Hider includes LiveShell Engine.

Third-party code: [stb_image / stb_image_write](https://github.com/nothings/stb) (public domain) in `third_party/stb`. One line in `stb_image_write.h` (JPEG bit writer) is changed to keep a shift within `int` range; it is marked "Home Hider".
