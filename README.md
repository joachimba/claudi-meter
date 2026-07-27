```text
  ____ _        _   _   _ ____ ___    __  __ _____ _____ _____ ____
 / ___| |      / \ | | | |  _ \_ _|  |  \/  | ____|_   _| ____|  _ \
| |   | |     / _ \| | | | | | | |   | |\/| |  _|   | | |  _| | |_) |
| |___| |___ / ___ \ |_| | |_| | |   | |  | | |___  | | | |___|  _ <
 \____|_____/_/   \_\___/|____/___|  |_|  |_|_____| |_| |_____|_| \_\
```

# Claudi Meter

A little ESP32 desk companion for Claude Code. It runs on a
[Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786)
(480×480 AMOLED) and pairs with your Mac or Linux box over Bluetooth. A host
daemon reads your Claude usage and a few other bits of desk-side context and
streams them to the display; the device shows them across a set of cycling
screens. The two side buttons double as a BLE keyboard for Claude Code's
voice-mode and mode-toggle shortcuts.

The splash screen plays pixel-art Clawd animations that get busier as your
usage rate climbs — sourced from [claudepix](https://claudepix.vercel.app),
[@amaanbuilds](https://x.com/amaanbuilds)'s lovely Clawd sprite library.

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
|   ![Usage meter](assets/demo.jpeg)    |    ![Clawd animation screen](assets/demo.gif)    |

## What's included

**On the device** — a stack of screens you cycle with the middle button
(and auto-rotate through when idle):

- **Splash** — pixel-art Clawd; the animation mood tracks your usage rate.
- **Usage** — 5-hour session and 7-day weekly rate-limit utilization, with reset countdowns.
- **History** — a smooth 30-day graph of daily token usage, with today's total, the day-over-day delta, and the equivalent API cost (`~$X today` / `$X / 30D`, the ccusage number).
- **Vacation** — a Polarsteps-style trip countdown: a full-bleed destination photo (pulled from Unsplash) behind a big "days until" number, the destination, and the date.
- **Spotify** — now-playing track + album art, with the side buttons wired to play/pause and skip.
- **Calendar** — your next event and time until it starts.
- **Pomodoro** — a tap-to-start focus timer.
- **Photo** — rotating ambient scenic backgrounds.
- **Bluetooth accessories** — battery levels of your other paired BT gear.
- **Settings / Bluetooth** — auto-rotate toggle, connection status, bond reset.

**On the host** — a companion daemon (+ optional menu-bar app) that polls
your Claude usage, aggregates daily token history and cost from your local
`~/.claude` transcripts, and gathers the extras above (Spotify, calendar,
photos, vacation image), pushing everything to the device over BLE.

> Not every screen needs the host — usage/history/vacation come from the
> daemon; Spotify/calendar are macOS-only extras. It's a personal build, so
> pick and choose.

## Hardware

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) — ESP32-S3R8, 2.16" 480×480 AMOLED (CO5300 QSPI), CST9220 cap touch, AXP2101 PMU + Li-Po battery, QMI8658 IMU
- USB-C cable for flashing and charging
- 3.7V Li-Po battery (MX1.25 2-pin connector, optional — runs off USB otherwise)

## Prerequisites

- macOS or Linux (tested on Ubuntu)
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html) (`pio`) for building/flashing
- Claude Code with an active subscription (the daemon reads its OAuth token)
- macOS: `python3` (the installer builds a venv with `bleak` + `httpx`)
- Linux: `curl`, `bluetoothctl`, `busctl` (BlueZ)

---

## Install & flash the device

### macOS

```bash
./flash-mac.sh                       # auto-detects /dev/cu.usbmodem*
./flash-mac.sh /dev/cu.usbmodem1101  # or pass an explicit port
```

Or straight with PlatformIO:

```bash
pio run -d firmware -t upload --upload-port /dev/cu.usbmodem1101
```

Then pair it: **System Settings → Bluetooth**, connect to **"Claude
Controller"**. The daemon discovers it on its next scan (~30 s).

> If you reflash after changing the BLE service (adding a characteristic),
> macOS caches the old GATT table. Because the device also bonds as a HID
> keyboard, a Bluetooth toggle isn't enough — **forget the device** in
> Bluetooth settings so it re-pairs and re-discovers.

### Linux

```bash
pio run -d firmware -t upload --upload-port /dev/ttyACM0
```

Pair once (the MAC is shown on the device's Bluetooth screen):

```bash
bluetoothctl scan le
bluetoothctl pair  <MAC>     # "Claude Controller"
bluetoothctl trust <MAC>
```

---

## Install the desktop companion

The daemon reads your Claude OAuth token (macOS Keychain service
`Claude Code-credentials`; Linux `~/.claude/.credentials.json`), polls usage
every 60 s, and pushes it — plus history, cost, and the extras — to the
display over BLE.

### macOS

```bash
./install-mac.sh
```

Creates a venv in `daemon/.venv/`, installs `bleak` + `httpx`, renders a
LaunchAgent to `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`,
and loads it (interactively the first time, so macOS prompts for Bluetooth
permission).

Control it with `clawd-ctl.sh` (alias it as `clawd`):

```bash
daemon/clawd-ctl.sh start | stop | restart | status | logs
```

### Linux

```bash
./install.sh
systemctl --user start claude-usage-daemon
systemctl --user status claude-usage-daemon
journalctl --user -u claude-usage-daemon -f
```

### Menu-bar app (macOS, optional)

[SwiftBar](https://github.com/swiftbar/SwiftBar) gives you a menu-bar control
for the daemon and the vacation countdown.

```bash
brew install --cask swiftbar
# Point SwiftBar's plugin folder at the repo (first launch asks), or symlink:
ln -sf "$PWD/daemon/swiftbar/Clawdmeter.30s.sh" <your-swiftbar-plugin-folder>/
open -a SwiftBar
```

The menu shows daemon status (start/stop/restart) and the current trip, with
**Set vacation… / Change trip… / Clear trip** and **Set Unsplash key…**.

### Vacation countdown setup

The trip is configured on the host (the device has no keyboard). Either use
the SwiftBar menu, or the helper script:

```bash
daemon/clawd-vacation.sh set     # prompts for destination + date, writes vacation.json
daemon/clawd-vacation.sh key     # store your Unsplash access key
daemon/clawd-vacation.sh clear   # remove the trip
```

Under the hood it writes `~/.config/claude-usage-monitor/vacation.json`
(`{"destination": "...", "date": "YYYY-MM-DD"}`). For the backdrop photo,
register a free key at [unsplash.com/developers](https://unsplash.com/developers)
and store it in `~/.config/claude-usage-monitor/unsplash-key` (or the
`UNSPLASH_ACCESS_KEY` env var). The daemon searches the destination, skips
photos tagged people/animals, prefers landscapes/cityscapes, and streams a
480×480 crop to the device. No key = countdown only, on a plain background.

---

## How it works

1. The daemon reads your Claude Code OAuth token (Keychain / credentials file).
2. It makes a minimal `api.anthropic.com/v1/messages` call — one Haiku token, basically free — and reads the usage numbers straight out of the `anthropic-ratelimit-unified-*` response headers.
3. For the History screen it aggregates daily tokens + equivalent cost from your local `~/.claude/projects/**/*.jsonl` transcripts (deduped by message id, priced per model).
4. It connects to the ESP32 over BLE and writes JSON payloads to the GATT RX characteristic; larger assets (album art, ambient photos, the vacation backdrop) stream over their own chunked characteristics.
5. The firmware parses everything and updates the LVGL screens; the splash animation mood follows the usage rate of change.
6. The two side buttons are independent — they send Space and Shift+Tab as BLE HID keyboard reports to the paired host.

## Physical buttons

| Button           | GPIO         | Function                                                          |
| ---------------- | ------------ | ----------------------------------------------------------------- |
| **Left**         | GPIO 0       | Hold to send Space (Claude Code voice-mode push-to-talk)          |
| **Middle** (PWR) | AXP2101 PKEY | Cycle screens; on splash, cycle animations                        |
| **Right**        | GPIO 18      | Send Shift+Tab (Claude Code mode toggle)                          |

Space and Shift+Tab go out as standard BLE HID reports, so they land in
whatever window has focus on the paired host.

## BLE protocol

The device advertises a custom GATT service alongside the standard HID
keyboard service:

|                              | UUID                                   |
| ---------------------------- | -------------------------------------- |
| **Data Service**             | `4c41555a-4465-7669-6365-000000000001` |
| RX (host writes JSON)        | `4c41555a-4465-7669-6365-000000000002` |
| TX (device ack notify)       | `4c41555a-4465-7669-6365-000000000003` |
| REQ (device refresh request) | `4c41555a-4465-7669-6365-000000000004` |
| IMG (album art chunks)       | `4c41555a-4465-7669-6365-000000000005` |
| CMD (device→host commands)   | `4c41555a-4465-7669-6365-000000000006` |
| PHOTO (ambient photo chunks) | `4c41555a-4465-7669-6365-000000000007` |
| VACIMG (vacation backdrop)   | `4c41555a-4465-7669-6365-000000000008` |
| **HID Service**              | `00001812-0000-1000-8000-00805f9b34fb` |

The RX payload is a single JSON object; the daemon sends only the keys that
changed. Common keys:

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true }
```

| Keys | Screen |
| --- | --- |
| `s`,`sr`,`w`,`wr`,`st`,`ok` | Usage (session/weekly %, reset minutes, status) |
| `tk`,`ck` | History (daily tokens in 100k units; cost in cents) |
| `vac` `{dst,dl,d,a}` | Vacation (destination, date label, days, active) |
| `spt`,`spa`,`spp`,`spc` | Spotify (track, artist, playing, colour) |
| `cat`,`cam` | Calendar (title, minutes until) |
| `pht` | Photo caption |
| `bta` | Bluetooth accessory list |
| `afk`,`attn` | AFK dim / attention bounce |

Big assets (`IMG`, `PHOTO`, `VACIMG`) use a chunked `[hash][offset][total]`
header so half-megabyte RGB565 frames fit.

## Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated file must be patched for LVGL 9:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## Converting Lucide icons

The UI uses a small set of [Lucide](https://lucide.dev) icons (bluetooth + battery states) converted to RGB565 / RGB565A8 C arrays for LVGL.

```bash
node tools/png_to_lvgl.js assets/icon_bluetooth_48.png icon_bluetooth_data ICON_BLUETOOTH_WIDTH ICON_BLUETOOTH_HEIGHT
```

Default tint is white (`0xFFFFFF`); Lucide PNGs ship as black-on-transparent and would render invisible against the dark UI without it. Pass `--no-tint` for pre-coloured artwork like the logo. Battery icons use RGB565A8 (alpha plane) so they blend cleanly over the splash; the rest are baked RGB565 over the panel colour. Paste the converter output into `firmware/src/icons.h`.

## Splash animations

The animations come from [claudepix.vercel.app](https://claudepix.vercel.app),
a library of Clawd sprites. `tools/scrape_claudepix.js` evaluates the site's
JavaScript in a Node VM to pull frame data and palettes, then
`tools/convert_to_c.js` turns everything into RGB565 C arrays and writes
`firmware/src/splash_animations.h`.

```bash
node tools/scrape_claudepix.js
node tools/convert_to_c.js
pio run -d firmware -t upload
```

See `tools/README.md` for details.

## Credits

- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app).
- Vacation backdrops via the [Unsplash API](https://unsplash.com/developers).
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery glyphs.
- macOS host port originally by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson).
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission, as well as using assets from Anthropic such as the copyrighted Clawd mascot. So even though the code in this repo is non-proprietary I will not license it myself under a copyleft license, since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
