#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Claude Controller" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

# PEP 604 annotations (`str | None`) — deferred-eval on 3.9 (system python).
from __future__ import annotations

import asyncio
import getpass
import io
import json
import os
import re
import signal
import struct
import subprocess
import sys
import time
import zlib
from datetime import datetime, timedelta
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
IMG_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000005"
CMD_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000006"
PHOTO_CHAR_UUID = "4c41555a-4465-7669-6365-000000000007"

# Ambient photo screen sizing + cadence.
PHOTO_W = 480
PHOTO_H = 480
PHOTO_BYTES = PHOTO_W * PHOTO_H * 2
PHOTO_CHUNK_PAYLOAD = 226       # MTU(247) - ATT(3) - photo header(10) margin
PHOTO_CHANGE_INTERVAL_S = 90    # rotate to a new photo every 90 s
PHOTO_LIST_URL = "https://chromecastbg.alexmeub.com/images.v9.json"

AFK_IDLE_THRESHOLD_S = 180     # 3 min of no input → user is gone
IDLE_TIME_RE = re.compile(r'"HIDIdleTime"\s*=\s*(\d+)')

# Attention flag — Claude Code (or any tool) can `touch` this file to make the
# dashboard's top-left Clawd bounce until the user reaches the device. Recommended
# hook in ~/.claude/settings.json:
#   {"hooks": {"Notification": [{"matcher":"*","hooks":[{"type":"command",
#    "command":"mkdir -p ~/.config/claude-usage-monitor && touch
#     ~/.config/claude-usage-monitor/attn.flag"}]}],
#              "Stop":         [{"matcher":"*","hooks":[{"type":"command",
#    "command":"mkdir -p ~/.config/claude-usage-monitor && touch
#     ~/.config/claude-usage-monitor/attn.flag"}]}]}}
ATTN_FLAG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "attn.flag"
ATTN_AUTO_CLEAR_S = 10          # bounce for at most 10 s before auto-resetting

# Device-initiated command codes (must match firmware/src/ble.h).
CMD_SPOTIFY_PLAYPAUSE = 0x01
CMD_SPOTIFY_NEXT      = 0x02
CMD_SPOTIFY_PREV      = 0x03

SPOTIFY_CMD_SCRIPTS = {
    CMD_SPOTIFY_PLAYPAUSE: 'tell application "Spotify" to playpause',
    CMD_SPOTIFY_NEXT:      'tell application "Spotify" to next track',
    CMD_SPOTIFY_PREV:      'tell application "Spotify" to previous track',
}

ART_W = 120
ART_H = 120
ART_BYTES = ART_W * ART_H * 2
ART_CHUNK_PAYLOAD = 230  # MTU(247) - ATT_overhead(3) - frame_header(6) = 238; 230 safe

POLL_INTERVAL = 60
TICK = 3
SPOTIFY_TICK = 3   # how often to poll Spotify.app between full API polls
SCAN_TIMEOUT = 8.0
CONNECT_TIMEOUT = 20.0   # bound client.connect(); a half-open link must not hang forever
BLE_WRITE_TIMEOUT = 10.0  # bound every GATT write; a stuck write -> teardown + reconnect
WATCHDOG_STALL_S = 45.0   # no main-loop progress this long -> force disconnect (unwedge)

# --- Daily token-usage history (for the firmware graph screen) ---
CLAUDE_PROJECTS_DIR = Path.home() / ".claude" / "projects"
HISTORY_DAYS = 30          # days of history shown on the graph
HISTORY_INTERVAL_S = 300   # recompute + push the token history at most this often
TOKEN_UNIT = 100_000       # graph values sent in units of 100k tokens (fits uint16)

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

SPOTIFY_SCRIPT = '''
tell application "System Events"
    if not (exists (processes where name is "Spotify")) then return ""
end tell
tell application "Spotify"
    try
        set s to player state as string
        set t to name of current track
        set a to artist of current track
        set u to ""
        try
            set u to artwork url of current track
        end try
        return s & "|||" & t & "|||" & a & "|||" & u
    on error
        return ""
    end try
end tell
'''

ICALBUDDY_ARGS = [
    "icalBuddy",
    "-nc",
    "-nrd",
    "-ea",
    "-b", "",
    "-eep", "notes,location,url,attendees",
    "-tf", "%H:%M",
    "-df", "%Y-%m-%d",
    "eventsToday+1",
]
ICAL_DATETIME_RE = re.compile(r"(\d{4}-\d{2}-\d{2}) at (\d{2}:\d{2})")

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


def get_spotify() -> dict | None:
    """Query Spotify.app via AppleScript. None if not running or unavailable."""
    if sys.platform != "darwin":
        return None
    try:
        out = subprocess.run(
            ["osascript", "-e", SPOTIFY_SCRIPT],
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    line = out.stdout.strip()
    if not line:
        return None
    parts = line.split("|||")
    if len(parts) < 3:
        return None
    state = parts[0].strip()
    track = parts[1].strip()
    artist = parts[2].strip()
    art_url = parts[3].strip() if len(parts) >= 4 else ""
    return {
        "playing": state.lower() == "playing",
        "track": track,
        "artist": artist,
        "art_url": art_url,
    }


_EVENTKIT_STORE = None
_EVENTKIT_AUTH = None    # None=untried, True=granted, False=denied/failed

def _eventkit_store():
    """Lazily build a single EKEventStore + request full access once.

    On macOS Sonoma+ this is the API path that respects the daemon's Python
    bundle Info.plist (NSCalendarsFullAccessUsageDescription) — running the
    CLI icalBuddy binary is unreliable on Tahoe because it has no embedded
    usage description and TCC silently denies it.
    """
    global _EVENTKIT_STORE, _EVENTKIT_AUTH
    if _EVENTKIT_STORE is not None:
        return _EVENTKIT_STORE if _EVENTKIT_AUTH else None
    try:
        from EventKit import EKEventStore, EKEntityTypeEvent
    except ImportError as e:
        log(f"EventKit import failed: {e}")
        _EVENTKIT_AUTH = False
        return None

    store = EKEventStore.alloc().init()
    granted_flag = {"ok": False, "done": False}

    def _completion(granted, err):
        granted_flag["ok"] = bool(granted)
        granted_flag["done"] = True

    # Prefer the macOS 14+ full-access API if present.
    sel = (
        "requestFullAccessToEventsWithCompletion:"
        if hasattr(store, "requestFullAccessToEventsWithCompletion_")
        else "requestAccessToEntityType:completion:"
    )
    if sel == "requestFullAccessToEventsWithCompletion:":
        store.requestFullAccessToEventsWithCompletion_(_completion)
    else:
        store.requestAccessToEntityType_completion_(EKEntityTypeEvent, _completion)

    # Block briefly for the completion handler. Returning quickly with no
    # grant is fine — we'll re-evaluate on the next call.
    from Foundation import NSRunLoop, NSDate
    rl = NSRunLoop.currentRunLoop()
    deadline = NSDate.dateWithTimeIntervalSinceNow_(2.0)
    while not granted_flag["done"] and NSDate.date().compare_(deadline) != 1:
        rl.runMode_beforeDate_(
            "NSDefaultRunLoopMode",
            NSDate.dateWithTimeIntervalSinceNow_(0.05),
        )
    _EVENTKIT_AUTH = granted_flag["ok"]
    _EVENTKIT_STORE = store
    log(f"EventKit access granted={_EVENTKIT_AUTH}")
    return store if _EVENTKIT_AUTH else None


def get_calendar() -> dict | None:
    """Return the next upcoming event via EventKit (with icalBuddy fallback).

    The EventKit path lives entirely inside the daemon's Python process so
    TCC keys off the patched Python.app Info.plist. icalBuddy stays as a
    fallback for older macOS versions where EventKit/pyobjc isn't usable.
    """
    if sys.platform != "darwin":
        return None
    store = _eventkit_store()
    if store is not None:
        try:
            from Foundation import NSDate
            now = NSDate.date()
            end = now.dateByAddingTimeInterval_(48 * 3600)  # next 48h horizon
            cals = store.calendarsForEntityType_(0)  # EKEntityTypeEvent = 0
            predicate = store.predicateForEventsWithStartDate_endDate_calendars_(
                now, end, cals
            )
            events = store.eventsMatchingPredicate_(predicate) or []
            # Filter out all-day events and pick the earliest by start date.
            future = [e for e in events if not e.isAllDay()]
            future.sort(key=lambda e: e.startDate())
            for ev in future:
                title = str(ev.title() or "")
                start = ev.startDate()
                secs = start.timeIntervalSinceNow()
                mins_until = 0 if secs < 0 else int(round(secs / 60.0))
                if title:
                    return {"title": title, "mins_until": mins_until}
            return None
        except Exception as e:
            log(f"EventKit query failed: {e}")

    # ---- icalBuddy fallback ----
    try:
        out = subprocess.run(
            ICALBUDDY_ARGS, capture_output=True, text=True, timeout=8
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    text = out.stdout.strip()
    if not text:
        return None
    title = None
    start_dt = None
    for raw in text.splitlines():
        if not raw.strip():
            continue
        if not raw.startswith((" ", "\t")):
            if title is None:
                title = raw.strip()
            else:
                break
            continue
        if start_dt is None:
            m = ICAL_DATETIME_RE.search(raw)
            if m:
                try:
                    start_dt = datetime.strptime(
                        f"{m.group(1)} {m.group(2)}", "%Y-%m-%d %H:%M"
                    )
                except ValueError:
                    pass
    if not title:
        return None
    if start_dt is None:
        mins_until = -1
    else:
        delta = (start_dt - datetime.now()).total_seconds() / 60.0
        mins_until = 0 if delta < 0 else int(round(delta))
    return {"title": title, "mins_until": mins_until}


def get_bt_accessories() -> list:
    """Return up to 6 connected Bluetooth accessories with battery info.

    Pulls from `system_profiler SPBluetoothDataType -json`. We only surface
    devices that actually expose a battery level — skips the dashboard's own
    "Claude Controller" and accessories without a battery field. Each entry
    is {"n": name, "b": pct (-1=unknown), "c": charging-bool}.
    """
    if sys.platform != "darwin":
        return []
    try:
        out = subprocess.run(
            ["system_profiler", "SPBluetoothDataType", "-json"],
            capture_output=True, text=True, timeout=6,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    try:
        data = json.loads(out.stdout)
    except json.JSONDecodeError:
        return []

    items = []
    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "device_connected" and isinstance(v, list):
                    for ent in v:
                        if isinstance(ent, dict):
                            for name, props in ent.items():
                                if isinstance(props, dict):
                                    items.append((name, props))
                else:
                    walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)

    def parse_pct(s):
        if not s:
            return -1
        m = re.search(r"(\d+)", str(s))
        return int(m.group(1)) if m else -1

    accessories = []
    for name, props in items:
        if name == "Claude Controller":
            continue
        main = props.get("device_batteryLevelMain")
        left = props.get("device_batteryLevelLeft")
        right = props.get("device_batteryLevelRight")
        case_pct = props.get("device_batteryLevelCase")
        pct = parse_pct(main)
        if pct < 0:
            # Average left + right for AirPods-style devices.
            l = parse_pct(left)
            r = parse_pct(right)
            if l >= 0 and r >= 0:
                pct = (l + r) // 2
            elif l >= 0:
                pct = l
            elif r >= 0:
                pct = r
        charging = (str(props.get("device_isCharging") or "").lower() == "yes")
        if pct < 0 and parse_pct(case_pct) < 0:
            continue   # nothing to show
        accessories.append({
            "n": name[:30],
            "b": pct,
            "c": charging,
        })
        if len(accessories) >= 6:
            break
    return accessories


def macos_idle_seconds() -> int:
    """Seconds since the last HID input (mouse / keyboard) via IORegistry.
    Returns 0 if the platform isn't macOS or `ioreg` is unavailable."""
    if sys.platform != "darwin":
        return 0
    try:
        out = subprocess.run(
            ["ioreg", "-c", "IOHIDSystem", "-d", "1", "-r", "-w", "0"],
            capture_output=True, text=True, timeout=4,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return 0
    m = IDLE_TIME_RE.search(out.stdout)
    if not m:
        return 0
    return int(m.group(1)) // 1_000_000_000   # ns -> s


def fetch_extras() -> tuple[dict, str | None]:
    """Build the JSON-payload extras and (separately) the current art URL."""
    extras: dict = {}
    art_url: str | None = None
    sp = get_spotify()
    if sp is not None:
        extras["spp"] = sp["playing"]
        extras["spt"] = sp["track"][:60]
        extras["spa"] = sp["artist"][:40]
        if sp.get("art_url"):
            art_url = sp["art_url"]
    cal = get_calendar()
    if cal is not None:
        extras["cat"] = cal["title"][:60]
        extras["cam"] = cal["mins_until"]
    return extras, art_url


def _url_hash16(url: str) -> int:
    return zlib.adler32(url.encode("utf-8")) & 0xFFFF


def _dominant_rgb(img) -> tuple[int, int, int]:
    """Pick a vibrant-but-not-extreme colour from an RGB image via adaptive
    palette quantisation. Skips near-black / near-white / desaturated swatches
    so the resulting tint reads as a 'mood' rather than the background."""
    from PIL import Image  # local import — only when we already have PIL
    p = img.convert("P", palette=Image.Palette.ADAPTIVE, colors=8)
    palette = p.getpalette()
    counts = sorted(p.getcolors() or [], reverse=True)  # [(count, idx), ...]
    for cnt, idx in counts:
        r = palette[idx * 3]
        g = palette[idx * 3 + 1]
        b = palette[idx * 3 + 2]
        mx = max(r, g, b)
        mn = min(r, g, b)
        if mx < 40:          # too dark
            continue
        if mn > 220:         # too washed-out
            continue
        if mx - mn < 25:     # too desaturated
            continue
        return (r, g, b)
    if counts:
        idx = counts[0][1]
        return (palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2])
    return (60, 60, 60)


async def _fetch_photo_rgb565(url: str) -> tuple[bytes, int] | None:
    """Download URL + resize to PHOTO_W×PHOTO_H + pack as RGB565 LE."""
    try:
        from PIL import Image
    except ImportError:
        log("Pillow not installed; ambient photo disabled")
        return None
    try:
        async with httpx.AsyncClient(timeout=20.0, follow_redirects=True) as http:
            r = await http.get(url)
        r.raise_for_status()
    except httpx.HTTPError as e:
        log(f"Photo fetch failed: {e}")
        return None
    try:
        from PIL import ImageOps
        src = Image.open(io.BytesIO(r.content)).convert("RGB")
        # Object-fit: cover — scale to fill PHOTO_W x PHOTO_H + center-crop.
        rgb = ImageOps.fit(
            src, (PHOTO_W, PHOTO_H),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )
    except Exception as e:
        log(f"Photo decode failed: {e}")
        return None
    buf = bytearray(PHOTO_BYTES)
    px = rgb.load()
    i = 0
    for y in range(PHOTO_H):
        for x in range(PHOTO_W):
            r8, g8, b8 = px[x, y]
            v = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
            buf[i] = v & 0xFF
            buf[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(buf), 0


async def _fetch_art_rgb565(url: str) -> tuple[bytes, int] | None:
    """Download an artwork URL and return (RGB565 bytes, dominant_rgb24_int).

    Returns None on any failure. Dominant colour is a packed 24-bit integer
    (0xRRGGBB) so it travels cleanly through the existing JSON pipeline.
    """
    try:
        from PIL import Image
    except ImportError:
        log("Pillow not installed; album art disabled (pip install pillow)")
        return None
    try:
        async with httpx.AsyncClient(timeout=8.0) as http:
            r = await http.get(url)
        r.raise_for_status()
    except httpx.HTTPError as e:
        log(f"Art fetch failed: {e}")
        return None
    try:
        rgb = Image.open(io.BytesIO(r.content)).convert("RGB").resize(
            (ART_W, ART_H), Image.Resampling.LANCZOS
        )
    except Exception as e:  # PIL raises a grab-bag of exception types on bad data
        log(f"Art decode failed: {e}")
        return None
    dom_r, dom_g, dom_b = _dominant_rgb(rgb)
    dom_int = (dom_r << 16) | (dom_g << 8) | dom_b
    buf = bytearray(ART_BYTES)
    px = rgb.load()
    i = 0
    for y in range(ART_H):
        for x in range(ART_W):
            r8, g8, b8 = px[x, y]
            v = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
            buf[i] = v & 0xFF
            buf[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(buf), dom_int


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    """Find the Clawdmeter peripheral on macOS.

    The device pairs with macOS as a BLE HID keyboard, which means
    CoreBluetooth hides it from regular `scanForPeripherals` results. We
    have two complementary strategies:

      1. Ask CoreBluetooth for already-connected peripherals exposing our
         custom data service (`retrieveConnectedPeripheralsWithServices`).
         This is the path that succeeds once the HID pair is in place.
      2. Fall back to a normal scan — works when the device is advertising
         but no central is holding the connection yet.
    """
    # Strategy 1: connected-peripherals lookup via pyobjc.
    try:
        from CoreBluetooth import CBCentralManager, CBUUID
        import objc
        log("connected-peripheral lookup starting")
        cm = CBCentralManager.alloc().initWithDelegate_queue_(None, None)
        await asyncio.sleep(2.0)  # let CB warm up
        # Try both our custom data service AND the HID service (which is what
        # macOS actually paired against, so the OS *definitely* knows the peer
        # is offering that service).
        for svc_str in (SERVICE_UUID, "1812"):
            svc_uuid = CBUUID.UUIDWithString_(svc_str)
            peripherals = cm.retrieveConnectedPeripheralsWithServices_([svc_uuid])
            log(f"connected-peripheral lookup (svc={svc_str}): {len(peripherals)} match(es)")
            for p in peripherals:
                ident = str(p.identifier().UUIDString())
                name = p.name() or ""
                log(f"  connected-peripheral: {ident}  name={name!r}")
                if name == DEVICE_NAME or not name:
                    log(f"Found via connected list: {ident}")
                    return ident
    except Exception as e:
        log(f"connected-peripheral lookup unavailable: {e}")

    # Strategy 2: filtered scan on advertised service UUIDs. The firmware
    # puts HID (0x1812) in the primary adv packet and our custom data UUID
    # in the scan response, so we try both filters.
    for svc_uuid in ("00001812-0000-1000-8000-00805f9b34fb", SERVICE_UUID):
        log(f"Filtered scan on {svc_uuid} ({SCAN_TIMEOUT}s)...")
        try:
            filtered = await BleakScanner.discover(
                timeout=SCAN_TIMEOUT, service_uuids=[svc_uuid]
            )
            for d in filtered:
                log(f"  filtered: {d.address!s:40s} name={d.name!r}")
            for d in filtered:
                if d.name == DEVICE_NAME:
                    log(f"Found via filtered scan: {d.address}")
                    return d.address
        except Exception as e:
            log(f"Filtered scan unavailable: {e}")

    # Strategy 3: regular scan as a last resort.
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        log(f"  saw: {d.address!s:40s} name={d.name!r}")
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# Per-token USD rates by model family, matching ccusage. Base is the
# published $/1M input & output; cache write is 1.25x input (5-min TTL) or
# 2x (1-hour), cache read is 0.1x input. These are equivalent API costs —
# subscription/OAuth usage isn't billed per token, so the graph is a proxy.
def _model_rates(model: str) -> tuple[float, float, float, float, float]:
    """Return (input, output, cache_write_5m, cache_write_1h, cache_read)
    in USD per million tokens for the model's family."""
    m = (model or "").lower()
    if "opus" in m:
        inp, out = 5.0, 25.0
    elif "haiku" in m:
        inp, out = 1.0, 5.0
    elif "fable" in m or "mythos" in m:
        inp, out = 10.0, 50.0
    else:  # sonnet and default
        inp, out = 3.0, 15.0
    return (inp, out, inp * 1.25, inp * 2.0, inp * 0.1)


def _message_cost_usd(model: str, u: dict) -> float:
    r = _model_rates(model)
    cc = u.get("cache_creation") or {}
    cw5 = cc.get("ephemeral_5m_input_tokens", 0)
    cw1 = cc.get("ephemeral_1h_input_tokens", 0)
    if not (cw5 or cw1):   # older lines lack the breakdown — treat all as 5m
        cw5 = u.get("cache_creation_input_tokens", 0)
    return (
        u.get("input_tokens", 0) * r[0]
        + u.get("output_tokens", 0) * r[1]
        + cw5 * r[2]
        + cw1 * r[3]
        + u.get("cache_read_input_tokens", 0) * r[4]
    ) / 1_000_000.0


def _parse_file_daily(fp: Path) -> dict[str, list]:
    """Sum usage tokens and equivalent USD cost per calendar day for one
    session transcript. Returns {date: [tokens, cost_usd]}.

    Dedups by (message id, requestId) so streaming/retry duplicate lines
    aren't counted twice. Tokens = input + output + cache creation + cache
    read; cost uses per-model rates, matching how ccusage totals a day."""
    daily: dict[str, list] = {}
    seen: set[str] = set()
    try:
        with open(fp, "r", errors="replace") as f:
            for line in f:
                if '"usage"' not in line:   # cheap prefilter; most lines skip
                    continue
                try:
                    d = json.loads(line)
                except (json.JSONDecodeError, ValueError):
                    continue
                msg = d.get("message")
                if not isinstance(msg, dict):
                    continue
                u = msg.get("usage")
                if not isinstance(u, dict):
                    continue
                mid = msg.get("id") or ""
                key = f"{mid}:{d.get('requestId') or ''}"
                if mid and key in seen:
                    continue
                seen.add(key)
                day = (d.get("timestamp") or "")[:10]
                if len(day) != 10:
                    continue
                tok = (
                    u.get("input_tokens", 0)
                    + u.get("output_tokens", 0)
                    + u.get("cache_creation_input_tokens", 0)
                    + u.get("cache_read_input_tokens", 0)
                )
                cost = _message_cost_usd(msg.get("model", ""), u)
                bucket = daily.setdefault(day, [0, 0.0])
                bucket[0] += tok
                bucket[1] += cost
    except OSError:
        pass
    return daily


def compute_token_history(days: int = HISTORY_DAYS) -> tuple[list[int], list[int]]:
    """Scan all Claude Code transcripts, return the last `days` days as
    (tokens, cost_cents), oldest -> newest, missing days = 0. Blocking IO —
    call via an executor. A full scan of a few hundred MB is under a second."""
    totals: dict[str, list] = {}
    try:
        for fp in CLAUDE_PROJECTS_DIR.rglob("*.jsonl"):
            for day, (tok, cost) in _parse_file_daily(fp).items():
                bucket = totals.setdefault(day, [0, 0.0])
                bucket[0] += tok
                bucket[1] += cost
    except OSError as e:
        log(f"Token history scan error: {e}")
    today = datetime.now().date()
    tokens, cost_cents = [], []
    for i in range(days - 1, -1, -1):
        b = totals.get(str(today - timedelta(days=i)), [0, 0.0])
        tokens.append(b[0])
        cost_cents.append(int(round(b[1] * 100)))
    return tokens, cost_cents


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()
        self.last_art_hash: int | None = None
        self.last_spotify_key: str | None = None
        self.last_spotify_playing: bool | None = None
        self.last_calendar_key: str | None = None
        self.last_photo_hash: int | None = None
        self.last_photo_change_ms: float = 0.0
        self.photo_list_cache: list | None = None
        self.photo_task: asyncio.Task | None = None
        self.last_afk: bool | None = None
        self.attn_active = False
        self.attn_started_at = 0.0
        self.last_bta_signature: str | None = None
        self.last_bta_poll_at: float = 0.0
        # Watchdog: bumped whenever the main loop makes progress. If it goes
        # stale the watchdog task force-disconnects to unwedge a hung await.
        self.last_progress: float = time.time()
        self.last_history_at: float = 0.0
        self.last_history_sig: str | None = None

    def note_progress(self) -> None:
        self.last_progress = time.time()

    async def _ble_write(self, uuid: str, data: bytes, response: bool) -> None:
        """Every GATT write goes through here so a half-open link can't hang
        the loop forever — a stuck write raises TimeoutError and the caller
        tears the session down instead of blocking indefinitely."""
        await asyncio.wait_for(
            self.client.write_gatt_char(uuid, data, response=response),
            timeout=BLE_WRITE_TIMEOUT,
        )

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    def _on_cmd(self, _char, data: bytearray) -> None:
        if not data:
            return
        code = data[0]
        script = SPOTIFY_CMD_SCRIPTS.get(code)
        if not script:
            log(f"Unknown cmd 0x{code:02x}")
            return
        log(f"Device cmd 0x{code:02x} → {script!r}")
        try:
            subprocess.run(
                ["osascript", "-e", script],
                capture_output=True, text=True, timeout=4
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            log(f"osascript failed: {e}")

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
            log("Refresh subscription OK")
        except Exception as e:
            log(f"Refresh subscription unavailable: {type(e).__name__}: {e}")
        try:
            await self.client.start_notify(CMD_CHAR_UUID, self._on_cmd)
            log("Cmd subscription OK")
        except Exception as e:
            log(f"Cmd subscription unavailable: {type(e).__name__}: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self._ble_write(RX_CHAR_UUID, data, response=False)
            return True
        except (BleakError, asyncio.TimeoutError) as e:
            log(f"Write failed: {e}")
            return False

    async def maybe_send_token_history(self) -> None:
        """Every HISTORY_INTERVAL_S, recompute the daily token history off the
        BLE thread and push it as {"tk":[...]} (units of 100k tokens, oldest
        -> newest) only when it changed."""
        now = time.time()
        if self.last_history_at and now - self.last_history_at < HISTORY_INTERVAL_S:
            return
        self.last_history_at = now
        loop = asyncio.get_running_loop()
        try:
            raw, cost_cents = await loop.run_in_executor(
                None, compute_token_history, HISTORY_DAYS
            )
        except Exception as e:
            log(f"Token history compute failed: {e}")
            return
        vals = [min(round(t / TOKEN_UNIT), 65535) for t in raw]
        cents = [max(c, 0) for c in cost_cents]
        sig = ",".join(map(str, vals)) + "|" + ",".join(map(str, cents))
        if sig == self.last_history_sig:
            return
        # Two separate writes — a combined {tk,ck} payload (~380B) exceeds the
        # BLE MTU. The firmware has a single RX buffer, so pause between writes
        # to let its loop consume `tk` before `ck` overwrites it (otherwise the
        # token payload is lost and the graph never marks itself valid).
        ok_tk = await self.write_payload({"tk": vals})
        await asyncio.sleep(0.2)
        ok_ck = await self.write_payload({"ck": cents})
        if ok_tk and ok_ck:
            self.last_history_sig = sig
            log(f"Token history sent ({sum(raw):,} tok, "
                f"${sum(cost_cents)/100:,.0f} over {len(vals)}d)")

    async def quick_bt_acc_sync(self) -> None:
        """system_profiler is slow (~1 s), so poll the BT accessory list at
        most once every 30 s. Push when the list changes, log every poll
        for now so we can see whether the daemon's launchd context is
        getting truncated SP output."""
        now = time.time()
        if now - self.last_bta_poll_at < 30:
            return
        self.last_bta_poll_at = now
        accessories = get_bt_accessories()
        log(f"BT acc poll: {len(accessories)} device(s) -> {accessories}")
        sig = json.dumps(accessories, sort_keys=True, separators=(",", ":"))
        if sig == self.last_bta_signature:
            return
        if await self.write_payload({"bta": accessories}):
            self.last_bta_signature = sig
            log(f"BT accessories -> {len(accessories)} device(s) (sent)")

    async def quick_attention_sync(self) -> None:
        """Watch for a touch-file written by Claude Code hooks. When seen,
        flip the firmware's attention flag on; auto-clear after the timeout
        so the Clawd logo doesn't bounce forever."""
        now = time.time()
        flag_touched = False
        try:
            if ATTN_FLAG_FILE.exists():
                flag_touched = True
                ATTN_FLAG_FILE.unlink(missing_ok=True)
        except OSError:
            pass

        if flag_touched and not self.attn_active:
            self.attn_active = True
            self.attn_started_at = now
            await self.write_payload({"attn": True})
            log("Attention -> on (Claude Code hook)")
        elif self.attn_active and (now - self.attn_started_at) >= ATTN_AUTO_CLEAR_S:
            self.attn_active = False
            await self.write_payload({"attn": False})
            log("Attention -> off (auto-clear)")

    async def quick_afk_sync(self) -> None:
        """Push an AFK flag whenever HID idle time crosses the threshold."""
        idle_s = macos_idle_seconds()
        is_afk = idle_s >= AFK_IDLE_THRESHOLD_S
        if is_afk == self.last_afk:
            return
        if await self.write_payload({"afk": is_afk}):
            self.last_afk = is_afk
            log(f"AFK -> {is_afk} (idle={idle_s}s)")

    async def quick_calendar_sync(self) -> None:
        """Fast Calendar-only update. icalBuddy returns instantly; polling it
        every tick means newly-accepted invites land on the dashboard within
        a couple of seconds rather than after the full 60 s API cycle."""
        cal = get_calendar()
        if cal is None:
            key = "__none__"
            payload = {"cat": "", "cam": -1}
        else:
            key = f"{cal['title']}|{cal['mins_until']}"
            payload = {"cat": cal["title"][:60], "cam": cal["mins_until"]}
        if key == self.last_calendar_key:
            return
        if await self.write_payload(payload):
            self.last_calendar_key = key

    async def quick_spotify_sync(self) -> None:
        """Fast Spotify-only update for snappier track changes.

        Runs between the slow Anthropic polls. Sends only the Spotify-related
        JSON keys when the current track or playback state changes; the
        firmware's per-section gate keeps the other on-screen data intact.
        Album art is sent immediately when the artwork URL changes.
        """
        sp = get_spotify()
        if sp is None:
            return
        key = f"{sp['artist']}|{sp['track']}"
        if key == self.last_spotify_key and sp["playing"] == self.last_spotify_playing:
            return
        payload = {
            "spp": sp["playing"],
            "spt": sp["track"][:60],
            "spa": sp["artist"][:40],
        }
        if await self.write_payload(payload):
            self.last_spotify_key = key
            self.last_spotify_playing = sp["playing"]
        if sp.get("art_url"):
            await self.maybe_send_art(sp["art_url"])

    async def _fetch_photo_list(self) -> list | None:
        if self.photo_list_cache is not None:
            return self.photo_list_cache
        try:
            async with httpx.AsyncClient(timeout=15.0) as http:
                r = await http.get(PHOTO_LIST_URL)
            r.raise_for_status()
            data = r.json()
        except Exception as e:
            log(f"Photo list fetch failed: {e}")
            return None
        if not isinstance(data, list) or not data:
            return None
        self.photo_list_cache = data
        log(f"Photo list cached: {len(data)} entries")
        return data

    async def maybe_send_photo(self) -> None:
        """Schedule (non-blocking) a new ambient photo every
        PHOTO_CHANGE_INTERVAL_S. The actual chunk transfer runs as a
        background task so spotify / calendar quick-syncs keep flowing
        through the same BLE link."""
        now = time.time()
        if self.photo_task and not self.photo_task.done():
            return  # transfer already in flight
        if now - self.last_photo_change_ms < PHOTO_CHANGE_INTERVAL_S:
            return
        self.last_photo_change_ms = now   # mark immediately so we don't double-trigger
        self.photo_task = asyncio.create_task(self._send_photo_task())

    async def _send_photo_task(self) -> None:
        photos = await self._fetch_photo_list()
        if not photos:
            return

        import random
        entry = None
        pkg = None
        for _ in range(5):
            entry = random.choice(photos)
            raw_url = entry.get("url") or ""
            if not raw_url:
                continue
            # Rewrite the Google CDN sizing hint so we request something near
            # 480 px directly — saves bandwidth + resize work.
            url = re.sub(r"=s\d+-w\d+-h\d+[^?]*", "=s640-w640-h640-c", raw_url)
            url = re.sub(r"/s\d+/", "/s640/", url)
            pkg = await _fetch_photo_rgb565(url)
            if pkg is not None:
                break
        if pkg is None or entry is None:
            return
        rgb, _ = pkg
        raw_url = entry.get("url") or ""
        h = _url_hash16(raw_url)
        if h == self.last_photo_hash:
            return
        photographer = (entry.get("photographer") or "").strip()
        caption = photographer or "Chromecast Backgrounds"

        log(f"Sending photo (hash=0x{h:04x}, bytes={len(rgb)}, by={photographer!r})")
        try:
            # Write-with-response so CoreBluetooth/NimBLE both pace properly —
            # write_no_response on macOS silently drops most chunks, leaving
            # the ESP-side buffer half-filled and the canvas stuck on the
            # "loading" state forever.
            chunk_idx = 0
            for offset in range(0, len(rgb), PHOTO_CHUNK_PAYLOAD):
                chunk = rgb[offset:offset + PHOTO_CHUNK_PAYLOAD]
                header = struct.pack("<HII", h, offset, len(rgb))
                await self._ble_write(PHOTO_CHAR_UUID, header + chunk, response=True)
                chunk_idx += 1
                if (chunk_idx & 15) == 0:
                    await asyncio.sleep(0)   # yield to quick-syncs
            await self.write_payload({"pht": caption[:90]})
            self.last_photo_hash = h
            log("Photo sent")
        except (BleakError, asyncio.TimeoutError) as e:
            log(f"Photo send failed: {e}")

    async def maybe_send_art(self, art_url: str) -> None:
        h = _url_hash16(art_url)
        if h == self.last_art_hash:
            return
        result = await _fetch_art_rgb565(art_url)
        if result is None:
            return
        rgb, dom_int = result
        log(f"Sending album art (hash=0x{h:04x}, bytes={len(rgb)}, dom=0x{dom_int:06x})")
        try:
            for offset in range(0, len(rgb), ART_CHUNK_PAYLOAD):
                chunk = rgb[offset:offset + ART_CHUNK_PAYLOAD]
                header = struct.pack("<HHH", h, offset, len(rgb))
                await self._ble_write(IMG_CHAR_UUID, header + chunk, response=True)
            self.last_art_hash = h
            log("Album art sent")
        except (BleakError, asyncio.TimeoutError) as e:
            log(f"Art send failed: {e}")
            self.last_art_hash = None
            return
        # Send the dominant colour as a tiny follow-up payload so the firmware
        # can tint the panel immediately after the art arrives.
        await self.write_payload({"spc": dom_int})


async def connect_and_run(address: str, stop_event: asyncio.Event) -> bool:
    """Connect to a known address and poll until disconnected or stopped.

    Returns True if the connection was used successfully (so the caller
    keeps the cached address), False if the connection failed and the
    cache should be invalidated.
    """
    log(f"Connecting to {address}...")
    client = BleakClient(address)
    try:
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    async def _watchdog() -> None:
        """Backstop for any await that hangs (bleak internals, half-open link)
        without hitting a per-call timeout. If the main loop stops making
        progress, force a disconnect so the stuck await raises and the outer
        loop reconnects from a clean slate."""
        while client.is_connected and not stop_event.is_set():
            await asyncio.sleep(TICK)
            stall = time.time() - session.last_progress
            if stall > WATCHDOG_STALL_S:
                log(f"Watchdog: loop stalled {stall:.0f}s — forcing disconnect")
                try:
                    await client.disconnect()
                except (BleakError, asyncio.TimeoutError):
                    pass
                return

    watchdog = asyncio.create_task(_watchdog())
    last_poll = 0.0
    last_tick_at = time.time()
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            session.note_progress()
            # Sleep / wake detection — a tick gap much larger than TICK means
            # the host went to sleep. The BLE link is almost always in a
            # stale state after wake, so we tear down the client and let the
            # outer loop reconnect from a clean slate.
            gap = now - last_tick_at
            if last_tick_at > 0 and gap > TICK * 5:
                log(f"Sleep/wake gap={gap:.0f}s — tearing down BLE client")
                try:
                    SAVED_ADDR_FILE.unlink(missing_ok=True)
                except OSError:
                    pass
                break
            last_tick_at = now
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None:
                        extras, art_url = fetch_extras()
                        payload.update(extras)
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True
                            # Seed the quick-sync caches so we don't re-send
                            # the same data on the next tick.
                            if "spt" in extras:
                                session.last_spotify_key = (
                                    f"{extras.get('spa','')}|{extras.get('spt','')}"
                                )
                                session.last_spotify_playing = extras.get("spp")
                            if "cat" in extras:
                                session.last_calendar_key = (
                                    f"{extras.get('cat','')}|{extras.get('cam', -1)}"
                                )
                            if art_url:
                                await session.maybe_send_art(art_url)

            # Quick Spotify + Calendar sync — fires on every TICK between
            # full polls so track / event changes appear on the dashboard
            # within a couple of seconds.
            await session.quick_spotify_sync()
            await session.quick_calendar_sync()
            await session.quick_afk_sync()
            await session.quick_attention_sync()
            await session.quick_bt_acc_sync()
            await session.maybe_send_token_history()
            await session.maybe_send_photo()

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        # Cancel background tasks before tearing the link down so they don't
        # leak as "Task was destroyed but it is pending" against a dead client.
        watchdog.cancel()
        try:
            await watchdog
        except (asyncio.CancelledError, Exception):
            pass
        if session.photo_task and not session.photo_task.done():
            session.photo_task.cancel()
            try:
                await session.photo_task
            except (asyncio.CancelledError, Exception):
                pass
        try:
            await client.disconnect()
        except (BleakError, asyncio.TimeoutError):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    consec_connect_failures = 0
    while not stop_event.is_set():
        address = load_cached_address()
        if not address:
            address = await scan_for_device()
            if address:
                save_address(address)
                consec_connect_failures = 0
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                # Cap backoff lower so wake-from-sleep reconnects quickly.
                backoff = min(backoff * 2, 15)
                continue

        ok = await connect_and_run(address, stop_event)
        if not ok:
            consec_connect_failures += 1
            # Keep the cached UUID across transient failures (e.g. laptop
            # wake-from-sleep) — only nuke it after several attempts, since
            # macOS can hold the peer in a stale state for a few seconds.
            if consec_connect_failures >= 3:
                log("Connect failed 3x, invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
                consec_connect_failures = 0
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 15)
        else:
            backoff = 1
            consec_connect_failures = 0


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
