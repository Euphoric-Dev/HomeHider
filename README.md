<div align="center">

# Home Hider

**Make the PS5 home screen yours. One payload for all of it.**
Hide tiles, group games into folders, give any tile a new name, picture or background, and play a video as your wallpaper.

![PS5 payload](https://img.shields.io/badge/PS5-payload-0070d1)
![Tested on FW 4.51](https://img.shields.io/badge/tested-FW%204.51-2ea44f)
![Version 2.1.0](https://img.shields.io/badge/version-2.1.0-7a4bf2)
![License GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-555)
![Runs with Payload Manager](https://img.shields.io/badge/runs%20with-Payload%20Manager-555)

</div>

---

Home Hider is a payload (`.elf`) for jailbroken PS5 consoles. Start it from **Payload Manager**, and its menu opens in the PS5 browser, or on your phone or PC.

> [!IMPORTANT]
> **Your changes stay in place even when the PS5 isn't jailbroken.** Home Hider writes them into the console's own home-screen database, so they survive restarts and normal boots. You only need the jailbreak to open Home Hider and make changes.

Nothing is ever deleted. Hidden games still work, and every change can be undone.

> [!IMPORTANT]
> **DISCLAIMER: THIS WAS MADE WITH AI!**
## Features

| | |
|---|---|
| **Hide anything** | Games, apps and system tiles such as PlayStation Store, Media Gallery, PlayStation Plus, TV & Video, All Apps — and even Game Library / App Library, which the keep-hidden watchdog puts back if the PS5 restores them. |
| **Folders** | One tile that opens a Game Library-style page with the games inside, like folders on the PS4. |
| **Rename** | Give any tile the name you want. |
| **Custom pictures** | Replace any tile's icon. Transparent PNGs work too. |
| **Custom backgrounds** | Replace the big background shown when a tile is selected, for games, apps and folders. |
| **Live wallpaper** | Play a video behind the home screen, powered by the built-in [LiveShell Engine](https://github.com/ZiZc3/LiveShell) by ZiZc3. |
| **One home-screen list** | Your games, apps and folders in one list, in the same order as your home screen, grouped by tab. Hidden tiles sit greyed at the end. Folders appear automatically next to your games. |
| **Reorder in place** | Use the ▲▼ arrows on any tile to move it earlier or later on its tab, then **Save order**. Home Hider re-applies your order each time it runs. |
| **Move between tabs** | Open **Edit** on any tile or folder and move it to the other tab. A folder carries everything inside it; take a single app out of a folder to move it on its own. |
| **Open anything** | Start any game or app from the menu, including hidden ones and web apps such as Payload Manager. |
| **Keeps itself right** | If the PS5 brings back a tile, name, picture or background, Home Hider puts your choice back the next time it runs. |
| **Quiet Autoload** | Runs silently when Payload Manager's Autoload starts it, and opens the menu when you launch it yourself. |
| **Updates itself** | From GitHub, a USB drive, or a file from your phone or PC. |
| **Safety first** | Backs up the home-screen database before changing anything. |

## Requirements

- A jailbroken PS5 (tested on firmware **4.51**)
- [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) by itsPLK, with its ELF loader
- `HomeHider.elf` from the [Releases](../../releases) page

## Install

1. Start **Payload Manager** on the PS5.
2. Add `HomeHider.elf` in any one of these ways:
   - **Phone or PC:** open `http://<PS5-IP>:8084`, go to **Get Payloads > Upload ELF Payload**, and choose the file.
   - **USB drive:** copy the file to the top of an exFAT or FAT32 drive. Turn on **Settings > Scan USB Payloads**, then go to **Get Payloads > USB Storage > Copy to Internal**.
   - **FTP:** copy the file to `/data/pldmgr/payloads/`.
3. Launch **HomeHider** from the Payload Manager dashboard.

The menu opens in the PS5 browser. A notification shows the address for your phone or PC: `http://<PS5-IP>:8090`.

## Getting started

| To... | Do this |
|---|---|
| Hide a tile | Find it with the search box or the tab filters, then press **Hide**. **Show** brings it back. |
| Make a folder | Press **+ New folder**, give it a name, choose a tab and colour, tap the games, and press **Save folder**. |
| Rename a tile, or change its picture or background | Press **Edit** next to it. |
| Start a game or app | Press **Open**. It works for hidden ones too. |
| See your changes | Hiding and folders show after you open and close any game, or restart. Names, pictures and backgrounds show after **Refresh home screen now**. |

## Folders

A folder is one tile on the **Games** or **Media** tab. Selecting it opens a page that looks like the Game Library: pick a game and it starts.

- Games in a folder are hidden from the home screen. Take a game out and it comes back, unless you had hidden it before.
- Give the folder a colour, or choose the chequered swatch for **no colour** (see-through).
- Use the four games as the folder picture, or choose your own picture and background.
- Web apps such as Payload Manager can go in folders too. Selecting one opens its page.
- If another game is running, the folder page asks before closing it.

> [!NOTE]
> Home Hider serves the folder page, so it needs to be running for folders to open. Add it to Autoload (see [Run it automatically](#run-it-automatically)).

## Names, pictures and backgrounds

Press **Edit** next to any game, app or system tile:

- **Name:** type a new name, up to 64 characters.
- **Picture:** choose a picture. Tick **Keep see-through parts transparent** to keep a PNG's transparent areas. Otherwise they get a dark background.
- **Background:** choose a wide picture. Home Hider saves it in the same size and format as the game's own background, so the home screen reads it the same way. Saving takes up to 10 seconds.

Each part has a **Use the original** button. The game's own files are never touched: Home Hider only changes what the home screen shows, and it saves every original first.

<details>
<summary><b>Pictures guide</b></summary>

<br>

**Where pictures come from.** Home Hider copies the picture it uses, so you can unplug the drive afterwards.

- **USB drive:** put pictures anywhere on the drive (up to four folders deep), then press **Choose from USB drive**. PS5 screenshots show up too.
- **Phone or PC:** open the menu at `http://<PS5-IP>:8090` and use the file picker.
- **FTP:** put pictures in `/data/homehider/pictures`.

| | |
|---|---|
| Formats | PNG, JPG, BMP, GIF, TGA (up to 32 MB). Convert HEIC and WebP to JPG first. |
| Icons | Square, 512&times;512 or bigger. Other shapes are cropped from the middle. |
| Backgrounds | Wide, 16:9, ideally 3840&times;2160. Other shapes are cropped from the middle. |
| Phone photos | Turned upright automatically. |
| GIFs | The first frame is used. Home screen tiles can't play animation. |

</details>

## Live wallpaper

Home Hider comes with **[LiveShell Engine](https://github.com/ZiZc3/LiveShell) 1.5.2 by ZiZc3** built in, so one payload gives you video wallpapers too. You don't need to load `liveshell.elf` separately.

1. In the menu, switch **Live wallpaper** on. Home Hider starts LiveShell, and from then on starts it every time Home Hider runs.
2. Add videos: copy `.mp4` files to a `liveshell` folder on a USB drive, or upload them from your phone or PC on LiveShell's page (`http://<PS5-IP>:9050`).
3. Press **Use** on a video to make it your wallpaper. **Delete** removes a video from the PS5.

| | |
|---|---|
| Videos | `.mp4`, H.264 or HEVC, 8-bit, up to 4K, with or without sound |
| More settings | **Open LiveShell** opens LiveShell's own page: looks, sound, playlists, a video for each game, and Discover (download wallpapers) |
| Turning it off | Switch **Live wallpaper** off. LiveShell stops, and the home screen restarts with its normal background. |

> [!NOTE]
> If you already load `liveshell.elf` from Payload Manager, Home Hider sees that it's running and doesn't start a second copy. LiveShell's files are in `/data/LiveShell/`.

## Reorder the home screen

The PS5 sorts each tab by most-recently-played and has no manual reorder. The home-screen list shows your tiles in that same order, and lets you set your own instead.

1. Use the **▲ / ▼** arrows on any tile to move it earlier or later on its tab (hidden tiles stay greyed at the end).
2. Press **Save order**, then **Refresh home screen now**.
3. **Automatic order** goes back to the PS5's recency sort.

Because the PS5 re-sorts by recency, Home Hider re-applies your order every time it runs, so **add it to Autoload** to keep it. Everything is saved and fully reversible.

## Run it automatically

Add Home Hider to Payload Manager's Autoload so folders always work and your changes are kept:

1. In Payload Manager, go to **Autoload > Add Item to Sequence** and choose **HomeHider**.
2. In Home Hider, go to **Options > When Home Hider starts** and choose **Quiet when started by Autoload**.

From then on, the Autoload start runs silently in the background, and launching Home Hider yourself from Payload Manager opens the menu.

## Updating

Go to **Updates** in the menu:

- **USB:** copy the new `HomeHider.elf` to the top of a USB drive and plug it in.
- **GitHub:** enter this repository under **Update settings**. Downloads are checked against GitHub's SHA-256 checksum.
- **File:** upload it from your phone or PC.

Nothing installs until you press **Install**. The old version is kept in `/data/homehider/backup/`.

## Undo

| To undo | Do this |
|---|---|
| One tile | **Show**, or **Edit > Use the original…** |
| Everything you hid | **Advanced > Show everything I hid** |
| Everything | **Advanced > Restore original backup**, then restart the PS5 |
| Without the jailbreak | Safe Mode > **Rebuild Database** |

The first-run backup of the home-screen database is kept in `/data/homehider/backup/app.db.original`.

<details>
<summary><b>Troubleshooting</b></summary>

<br>

| Problem | Fix |
|---|---|
| A change doesn't show | Hiding and folders: open and close any game, or restart. Names, pictures and backgrounds: **Refresh home screen now**. |
| A folder shows "can't connect" | Home Hider isn't running. Start it, or add it to Autoload. |
| A web app in a folder shows "can't connect" | The payload behind it (for example, Payload Manager) isn't running. |
| The menu opens at Autoload | **Options > When Home Hider starts > Quiet when started by Autoload**. |
| "Choose from USB drive" finds nothing | Use exFAT or FAT32, keep pictures within four folders of the top, and press **Refresh**. |
| A background "can't be written yet" | That game uses an unusual texture format. |
| "Port 8090 is busy" | Restart the PS5 and launch Home Hider again. |
| The video stops after **Refresh home screen now** | Switch **Live wallpaper** off and on again. |
| Live wallpaper says "Starting" and nothing happens | Check that Payload Manager's ELF loader is running, then switch Live wallpaper off and on. If LiveShell itself shows an error, see its log in `/data/LiveShell/liveshell.log`. |
| Anything else | **Advanced > Diagnostics** shows a log of every step. Please include it when you open an issue. |

</details>

<details>
<summary><b>Questions</b></summary>

<br>

**Does it delete anything?** No. Hidden games stay installed, and every original value is saved so it can be put back.

**Do changes survive without the jailbreak?** Yes. Only a system update or Rebuild Database can undo them, and Home Hider puts them back the next time it runs.

**Can GIFs or backgrounds animate?** Tile pictures and backgrounds are still pictures. For a moving wallpaper, use [Live wallpaper](#live-wallpaper).

**Why can't Game Library and App Library be hidden?** The home screen adds those two tiles itself.

**Can I use it from my phone?** Yes. Open `http://<PS5-IP>:8090` on any device on the same network.

</details>

## Building from source

See [BUILDING.md](BUILDING.md). It covers the PS5 build, the desktop test suite (`make test`) and how the home-screen changes work.

## Credits

- **[LiveShell Engine](https://github.com/ZiZc3/LiveShell) by ZiZc3**: the live wallpaper engine, built into Home Hider unchanged (GPL-3.0). LiveShell's own credits: [OnionHEN](https://github.com/aydencharles/onionHEN) (payload injector, detour library and ShellUI groundwork) and the PS5 Payload SDK.
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom
- [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) by itsPLK
- Home-screen research: [PS5-TabShifter](https://github.com/xvvulcanvx/PS5-TabShifter), [psgfx](https://github.com/elripalda/psgfx), [PS5 Art Swap](https://github.com/griches/PS5_ArtSwap) and the [PS5 Developer Wiki](https://www.psdevwiki.com/ps5/Title_ID)
- [SQLite](https://sqlite.org) and [stb](https://github.com/nothings/stb) (public domain)

## License

Home Hider is free software under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)). It includes LiveShell Engine, which is GPL-3.0. LiveShell's source code is in [`third_party/liveshell/source`](third_party/liveshell/source), and its project lives at [github.com/ZiZc3/LiveShell](https://github.com/ZiZc3/LiveShell).

<div align="center">
<sub>Use at your own risk. Home Hider only changes the home-screen layout, keeps backups, and can undo everything it does.</sub>
</div>
