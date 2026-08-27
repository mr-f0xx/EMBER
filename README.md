<img width="1100" height="400" alt="Image" src="https://github.com/user-attachments/assets/cc78e87a-e181-499a-aee7-c9214cfb5a73" />

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-☕-orange)](https://buymeacoffee.com/horseyofcoursey)

A themeable MP3 / FLAC / WAV / AAC player firmware for the [M5Stack Cardputer ADV](https://shop.m5stack.com/products/m5stack-cardputer-adv-version-esp32-s3) (ESP32-S3). SD-card folder browsing, album art, custom color themes, and a couple of full-screen visualizers.

> **Ember+** — this fork adds 17 built-in themes, translates the UI into French, and shows **only the `Music` folder at the SD root** (it is created automatically on first boot; put your Artist → Album → Track tree inside it). Also adds 7 selectable Now Playing visualizer styles and **remote music playback from a Subsonic/Gonic server (key `w`)**.


<p float="left">
  <img src="docs/screenshots/now_playing.png" width="320" alt="Now Playing screen">
  <img src="docs/screenshots/file_navigation.png" width="320" alt="Folder browser">
  <img src="https://github.com/user-attachments/assets/57eebac1-4dfc-464e-8e84-12f8bde95d6e" width="320" alt="viusalizer_Image">
</p>

## In motion

| Full-screen dancer visualizer | Turntable placeholder | Theme cycling |
|:---:|:---:|:---:|
| ![Dancers](docs/screenshots/dancers.gif) | ![Turntable](docs/screenshots/record_spinning.gif) | <img width="330" alt="Image" src="https://github.com/user-attachments/assets/4868eb45-de1d-4689-ba93-1db2f00431b6" /> |

## Features

- **Folder browsing** straight off the SD card (Artist → Album → Track), natural-sorted, no library scan/database step.
- **MP3, FLAC, WAV, and AAC** playback.
- **Themeable UI** — 17 built-in themes (Ember, 90's Sweater, Aqua, Honey, Moody, Terminal Green, Tokyo Night, Dracula, Gruvbox, Catppuccin, Rosé Pine, Monokai, Nord, Apprentice, Amber on Black, Zenburn, Tomorrow Night)
- **Selectable visualizer styles** (Settings → Visualiseur): 7 styles on the Now Playing screen — LED bars (classic), oscilloscope waveform, mini real-FFT spectrum, dual stereo channels, peak-hold bars, bass pulse orb, and mirrored center-out bars.
- **Two full-screen visualizers**: a real FFT spectrum analyzer (bars + peak-hold + waveform overlay + stereo level meter), and a full-screen silhouette dance visualizer that reacts to bass hits in the music.
- **Now Playing extras**: embedded album art (JPEG/PNG/BMP/QOI), an animated turntable placeholder for tracks with no art, a small amplitude visualizer, seek with double-tap-to-restart/skip, and battery/volume meters.
- **Settings**: backlight level, screen-off timeout, end-of-album behavior, and theme — all persisted across reboots.
- **On-device screenshot capture** (see [Screenshots](#taking-your-own-screenshots) below) for pulling real UI captures without photographing the screen.
- **Multi-language support** displays Japanese, Chinese, Cyrillic (Russian, etc.), Greek, and accented Latin (French, German, Spanish, etc). 

## Hardware

- M5Stack Cardputer ADV (ESP32-S3, no PSRAM).
- A microSD card for your music (and optionally custom themes — see below).

## Flashing the prebuilt firmware (Ember+)

`EmberPlus.bin` (repo root) is a **merged image** — bootloader, partition
table, and app in one file — flashable as-is at address `0x0`:

```bash
esptool.py --port COMx write_flash 0x0 EmberPlus.bin
```

No-command-line option: open <https://espressif.github.io/esp-launchpad/> in a
Chromium browser, pick ESP32-S3, address `0x0`, and paste the raw file URL
(`https://raw.githubusercontent.com/<your-account>/EMBER/main/EmberPlus.bin`)
or select the downloaded file.

The GitHub Actions build (every push and pull request) compiles the firmware,
merges bootloader + partition table + app with esptool, and publishes the
result as a downloadable artifact; every `v*` tag additionally creates a
**Release** with the `.bin` attached. Note: `ember-cardputer-adv.bin` at the
repo root is the ORIGINAL upstream EMBER firmware (no additions) — flash
`EmberPlus.bin` for the full Ember+ build.

## Controls

The Cardputer has no dedicated arrow keys — the punctuation cluster doubles as one: `;` `.` `,` `/` map to up/down/back/open.

| Key | Action |
|---|---|
| `;` / `.` | Move selection up/down in the file browser |
| `,` | Back / up a folder (at the root, opens Now Playing instead) |
| `/` | Open selected folder or track |
| `` ` `` | Back, from anywhere |
| Enter | Open / play |
| Space | Play / pause |
| `,` / `/` (Now Playing) | Seek back/forward; double-tap to restart / skip to next track |
| `n` / `b` | Next / previous track |
| `-` / `=` | Volume down / up |
| `m` | Toggle Now Playing screen |
| `v` (Now Playing) | Cycle full-screen visualizer: spectrum → dancers → back |
| `a` (Now Playing) | Toggle turntable placeholder vs. real album art |
| `s` | Settings |
| `c` | Save a screenshot to `/screenshots` on the SD card (hold to burst-capture) |
| `w` | Open / close the remote music player (Subsonic/Gonic) |

## Remote music (Subsonic / Gonic)

Press `w` to browse your self-hosted music server over WiFi: **Artists → Albums → Songs** (Subsonic API: Gonic, Navidrome, Airsonic, stock Subsonic…). The screen reuses EMBER's theme and row style; `;` / `.` move, Enter or `/` opens, `,` goes back up a level, `` ` `` or `w` (at the artist level) returns to the SD browser, and `m` toggles between the song list and Now Playing. Volume, pause, visualizers and screenshots work as usual, and `,` / `/` on Now Playing seek inside MP3s (HTTP Range) with double-tap for previous/next song.

Remote albums play sequentially (the end-of-album setting applies to the SD library; remote albums stop at the end and return to the song list). The Now Playing screen shows title / artist / album from the server's metadata and a framed placeholder in place of the cover art. MP3, AAC and FLAC streams are supported.

### Files on the SD card

| File | Content |
|---|---|
| `/wifi.txt` | WiFi credentials — line 1 SSID, line 2 password (see [`config/wifi.txt.example`](config/wifi.txt.example)) |
| `/subsonic.txt` | Server address, username, password — lines 1–3 (see [`config/subsonic.txt.example`](config/subsonic.txt.example)) |

If a file is missing or empty, the firmware falls back to the hardcoded values at the bottom of `src/network.h` (recompile needed). WiFi connects on demand (first `w` press) and is turned off when you leave the network player — SD playback and boot behavior are untouched. Authentication uses the Subsonic token scheme (`md5(password + salt)`), so the password itself never travels over the network.

## Custom themes

Every color in the UI — backgrounds, text, selection highlight, visualizer tiers, all of it — comes from one theme struct, editable without touching any drawing code.

**Make your own:** open the [theme editor](https://horseyofcoursey.github.io/EMBER/tools/theme-editor.html) in a browser. It shows a live, device-accurate preview (colors are quantized to the Cardputer's actual 16-bit display depth, so what you see is what you'll get) of the browser, Now Playing, and visualizer screens as you tweak each color.

**Use it on your device (no recompiling):** on the JSON tab, click **Download**, then copy the file into a `/themes` folder on your SD card. After a reboot, it shows up as an extra option under **Settings → Theme**.

The editor also works completely offline as a local file (`tools/theme-editor.html`) if you'd rather not use the hosted copy.

## Taking your own screenshots

Press `c` on any screen to save a BMP to `/screenshots` on the SD card; hold it to burst-capture a sequence (useful for the animated screens). There's no timestamp metadata (no RTC on this board), so if you need to tell capture sessions apart afterward, diffing consecutive frames for content changes works well.

## Credit
Thank you to the creators of the following repositories (in no particular order) that inspired this project and provided a code base to start.
[AdvanceOS-for-cardputer](https://github.com/bomberman30/AdvanceOS-for-cardputer) - bomberman30  
[MP3PlayerforM5Cardputer](https://github.com/sanchitminda/MP3PlayerForM5Cardputer) -  sanchitminda  
[CardPuter_Mp3_Adv](https://github.com/vicliu624/CardPuter_Mp3_Adv) - vicliu624  
[M5Mp3](https://github.com/VolosR/M5Mp3) - VolosR




## License

[MIT](LICENSE)

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-☕-orange)](https://buymeacoffee.com/horseyofcoursey)
