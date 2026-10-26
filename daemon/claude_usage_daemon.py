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
import threading
import time
import zlib
from datetime import datetime, timedelta
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
# macOS reports the *bonded* peer's cached/user-edited name, not the name in the
# advertisement, so a device renamed in Bluetooth Settings stops matching
# DEVICE_NAME exactly. Any of these count as ours; a hit on SERVICE_UUID counts
# regardless of name.
DEVICE_NAMES = {DEVICE_NAME, "Clawdmeter", "Claudi Meter"}
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"
IMG_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000005"
CMD_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000006"
VACIMG_CHAR_UUID = "4c41555a-4465-7669-6365-000000000008"

# Full-screen vacation backdrop sizing.
BACKDROP_W = 480
BACKDROP_H = 480
BACKDROP_BYTES = BACKDROP_W * BACKDROP_H * 2
BACKDROP_CHUNK_PAYLOAD = 226    # MTU(247) - ATT(3) - backdrop header(10) margin

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
# --- Vacation countdown ---
CONFIG_DIR = Path.home() / ".config" / "claude-usage-monitor"
VACATION_FILE = CONFIG_DIR / "vacation.json"       # {"destination": "...", "date": "YYYY-MM-DD"}
UNSPLASH_KEY_FILE = CONFIG_DIR / "unsplash-key"    # one line: the access key (or $UNSPLASH_ACCESS_KEY)
VACATION_INTERVAL_S = 60     # re-evaluate the countdown at most this often
VACATION_TRIP_DAYS = 14      # vacation.json only has a departure date; show
                             # "under way" this long after it, then "no trip"
UNSPLASH_SEARCH = "https://api.unsplash.com/search/photos"
# Bias the backdrop toward places, not portraits/wildlife — applied by
# filtering the result set (not by narrowing the search query). Matched at
# WORD level (a token set), so "cat" won't hit "location" nor "men" "monument".
UNSPLASH_REJECT = frozenset((
    # people
    "people", "person", "persons", "portrait", "human", "humans", "man", "men",
    "woman", "women", "boy", "girl", "child", "children", "kid", "kids", "face",
    "crowd", "selfie", "model", "family", "couple", "tourist", "tourists",
    "hiker", "hikers", "girls", "boys", "lady",
    # animals
    "animal", "animals", "wildlife", "safari", "herd", "flock", "dog", "dogs",
    "cat", "cats", "pet", "pets", "bird", "birds", "horse", "horses", "lion",
    "lions", "elephant", "elephants", "giraffe", "zebra", "zebras", "leopard",
    "rhino", "antelope", "oryx", "springbok", "cheetah", "buffalo", "monkey",
    "baboon", "meerkat", "ostrich", "seal", "seals", "whale", "dolphin",
    "penguin", "penguins", "deer", "sheep", "cattle", "cow", "cows", "goat",
    "fox", "bear", "tiger", "snake", "insect", "butterfly", "fish", "flamingo",
    "flamingos",
))
UNSPLASH_PREFER = frozenset((
    "city", "cityscape", "skyline", "architecture", "building", "buildings",
    "landscape", "landscapes", "nature", "mountain", "mountains", "scenery",
    "street", "aerial", "landmark", "coast", "coastline", "skyscraper",
    "temple", "sea", "beach", "sunset", "desert", "dunes", "valley", "forest",
    "waterfall", "lake", "river", "cliff", "canyon", "travel", "architecture",
))

CONNECT_TIMEOUT = 20.0   # bound client.connect(); a half-open link must not hang forever
BLE_WRITE_TIMEOUT = 10.0  # bound every GATT write; a stuck write -> teardown + reconnect
WATCHDOG_STALL_S = 45.0   # no main-loop progress this long -> force disconnect (unwedge)
DISCONNECT_TIMEOUT = 5.0  # bound client.disconnect(); bleak waits on a CB callback that may never fire
SLEEP_GAP_S = 10.0        # wall clock outran awake time by this much -> host slept
LIVENESS_STALL_S = 300.0  # no heartbeat for this much awake time -> exit, launchd restarts us

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
# Peers that answered with CBError 14 ("Peer removed pairing information").
# macOS keeps a *second* CoreBluetooth identity for a device whose bond the
# peer has since dropped, and that ghost keeps showing up in scans forever
# alongside the live one. Persisted so a daemon restart doesn't relearn it.
BAD_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "bad-addresses"
# Last peer that actually delivered data. Unlike ble-address (a scan result,
# discarded on failure) this is only written after a working session and is
# never deleted, so a nearby look-alike board can't permanently displace the
# real one in the scan results.
LAST_GOOD_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "last-good-address"

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


def awake_clock() -> float:
    """Seconds of host uptime that excludes time spent asleep. Paired with
    time.time() (which does advance across sleep) to tell a real sleep apart
    from a loop iteration that was merely slow."""
    if hasattr(time, "CLOCK_UPTIME_RAW"):  # macOS: stops while asleep
        return time.clock_gettime(time.CLOCK_UPTIME_RAW)
    return time.monotonic()  # Linux CLOCK_MONOTONIC also stops in suspend


_last_heartbeat = awake_clock()


def heartbeat() -> None:
    global _last_heartbeat
    _last_heartbeat = awake_clock()


def start_liveness_watchdog() -> None:
    """Last line of defence against hangs nobody has found yet. Runs on its own
    thread so it still fires when the event loop is wedged (a blocking call) or
    idle on a future that never resolves (an unbounded await). Exits non-zero
    so launchd's KeepAlive restarts the daemon from a clean slate. Measured in
    awake time, so a night of host sleep doesn't count as a stall."""
    def _run() -> None:
        while True:
            time.sleep(15)
            stall = awake_clock() - _last_heartbeat
            if stall > LIVENESS_STALL_S:
                log(f"Liveness watchdog: no progress for {stall:.0f}s — exiting for restart")
                os._exit(1)

    threading.Thread(target=_run, name="liveness-watchdog", daemon=True).start()


async def safe_disconnect(client: BleakClient) -> None:
    try:
        await asyncio.wait_for(client.disconnect(), timeout=DISCONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError):
        pass


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


async def _fetch_backdrop_rgb565(url: str) -> bytes | None:
    """Download URL + resize to BACKDROP_W×BACKDROP_H + pack as RGB565 LE."""
    try:
        from PIL import Image, ImageOps
    except ImportError:
        log("Pillow not installed; vacation backdrop disabled")
        return None
    try:
        async with httpx.AsyncClient(timeout=20.0, follow_redirects=True) as http:
            r = await http.get(url)
        r.raise_for_status()
    except httpx.HTTPError as e:
        log(f"Backdrop fetch failed: {e}")
        return None
    try:
        src = Image.open(io.BytesIO(r.content)).convert("RGB")
        # Object-fit: cover — scale to fill BACKDROP_W x BACKDROP_H + center-crop.
        rgb = ImageOps.fit(
            src, (BACKDROP_W, BACKDROP_H),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )
    except Exception as e:
        log(f"Backdrop decode failed: {e}")
        return None
    buf = bytearray(BACKDROP_BYTES)
    px = rgb.load()
    i = 0
    for y in range(BACKDROP_H):
        for x in range(BACKDROP_W):
            r8, g8, b8 = px[x, y]
            v = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
            buf[i] = v & 0xFF
            buf[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(buf)


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


def load_bad_addresses() -> set[str]:
    try:
        return {ln.strip().upper() for ln in BAD_ADDR_FILE.read_text().splitlines()
                if ln.strip()}
    except OSError:
        return set()


def mark_bad_address(addr: str) -> None:
    """Remember a peer whose bond is dead so scans stop handing it back."""
    bad = load_bad_addresses()
    if addr.upper() in bad:
        return
    bad.add(addr.upper())
    BAD_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    BAD_ADDR_FILE.write_text("\n".join(sorted(bad)) + "\n")
    log(f"Blacklisted dead peer {addr}")


def load_last_good_address() -> str | None:
    try:
        addr = LAST_GOOD_ADDR_FILE.read_text().strip()
    except OSError:
        return None
    return addr or None


def save_last_good_address(addr: str) -> None:
    if load_last_good_address() == addr:
        return
    LAST_GOOD_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    LAST_GOOD_ADDR_FILE.write_text(addr)
    log(f"Recorded known-good peer {addr}")


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    if addr.upper() in load_bad_addresses():
        log(f"Cached address {addr} is blacklisted, discarding")
        SAVED_ADDR_FILE.unlink(missing_ok=True)
        return None
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


def _candidate_rank(name: str | None) -> int:
    """Lower is better. A peer advertising the *firmware's* name is the live
    device; a peer showing some other cached name is usually a ghost record
    macOS kept from an old bond (see BAD_ADDR_FILE), so try it last."""
    if name == DEVICE_NAME:
        return 0
    if name in DEVICE_NAMES:
        return 2
    return 1  # unnamed but matched on our service UUID


def _pick_candidate(cands: list[tuple[str, str | None]], how: str,
                    skipped: list[tuple[str, str | None]] | None = None) -> str | None:
    """Drop blacklisted peers, then take the best-ranked survivor. Blacklisted
    ones are appended to `skipped` so the caller can fall back to them if no
    strategy turns up anything better."""
    bad = load_bad_addresses()
    live: list[tuple[str, str | None]] = []
    for addr, name in cands:
        if addr.upper() in bad:
            log(f"  skipping blacklisted peer {addr} (name={name!r})")
            if skipped is not None and addr not in [a for a, _ in skipped]:
                skipped.append((addr, name))
        else:
            live.append((addr, name))
    if not live:
        return None
    live.sort(key=lambda an: _candidate_rank(an[1]))
    addr, name = live[0]
    log(f"Found via {how}: {addr} (name={name!r})")
    return addr


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

    Every strategy collects *all* matches and ranks them rather than taking
    the first hit: the Mac can surface two identities for one device (the live
    one plus a ghost of a dropped bond), and scan order between them is
    arbitrary.
    """
    # Peers we passed over because they're blacklisted; used as a last-resort
    # fallback below so a stale blacklist can never lock the daemon out.
    skipped: list[tuple[str, str | None]] = []

    # Strategy 1: connected-peripherals lookup via pyobjc.
    try:
        from CoreBluetooth import CBCentralManager, CBUUID
        log("connected-peripheral lookup starting")
        # Give the manager its own dispatch queue: with queue=None it delivers
        # on the main queue, which asyncio never services, so it can sit in
        # .unknown forever and every retrieve returns nothing.
        try:
            import libdispatch
            cb_queue = libdispatch.dispatch_queue_create(b"claudebuddy.cb", None)
        except Exception as e:
            log(f"libdispatch unavailable ({e}); falling back to main queue")
            cb_queue = None
        cm = CBCentralManager.alloc().initWithDelegate_queue_(None, cb_queue)
        # Poll for poweredOn (5) rather than sleeping blind — the lookup is
        # silently empty until the manager is up.
        for _ in range(60):
            if cm.state() == 5:
                break
            await asyncio.sleep(0.1)
        log(f"CBCentralManager state={cm.state()} (5=poweredOn)")
        # Our custom data service plus every standard service the HID profile
        # publishes (HID, device info, battery) — macOS definitely knows the
        # bonded keyboard offers those, and a peer it already holds a
        # connection to is invisible to scans.
        for svc_str in (SERVICE_UUID, "1812", "180A", "180F"):
            svc_uuid = CBUUID.UUIDWithString_(svc_str)
            peripherals = cm.retrieveConnectedPeripheralsWithServices_([svc_uuid])
            log(f"connected-peripheral lookup (svc={svc_str}): {len(peripherals)} match(es)")
            cands: list[tuple[str, str | None]] = []
            for p in peripherals:
                ident = str(p.identifier().UUIDString())
                name = p.name() or ""
                log(f"  connected-peripheral: {ident}  name={name!r}")
                # A hit on our custom 128-bit service is proof enough — nothing
                # else on the machine exposes it, whatever macOS calls it.
                if svc_str == SERVICE_UUID or name in DEVICE_NAMES or not name:
                    cands.append((ident, name))
            picked = _pick_candidate(cands, "connected list", skipped)
            if picked:
                return picked
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
            # Our custom service UUID is unique to this firmware, so any device
            # that answers that filter is the one we want regardless of the name
            # macOS has cached for it. The HID (0x1812) filter is NOT unique, so
            # there we still require a known name.
            cands = [(d.address, d.name) for d in filtered
                     if svc_uuid == SERVICE_UUID or d.name in DEVICE_NAMES]
            picked = _pick_candidate(cands, "filtered scan", skipped)
            if picked:
                return picked
        except Exception as e:
            log(f"Filtered scan unavailable: {e}")

    # Strategy 3: regular scan as a last resort.
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        log(f"  saw: {d.address!s:40s} name={d.name!r}")
    picked = _pick_candidate([(d.address, d.name) for d in devices
                              if d.name in DEVICE_NAMES], "scan", skipped)
    if picked:
        return picked

    # Nothing but blacklisted peers anywhere. The blacklist must be stale (the
    # live device dropped its bond after a reflash, then re-paired under the
    # same identity), so drop it and retry the best-ranked one.
    if skipped:
        if load_last_good_address():
            # A known-good peer exists, it's just not advertising right now
            # (macOS hides peripherals it already holds a connection to). Keep
            # the blacklist and let the caller retry the known-good address.
            log("Only blacklisted peers found — keeping blacklist, "
                "known-good peer will be retried")
            return None
        log("Only blacklisted peers found — clearing blacklist and retrying")
        BAD_ADDR_FILE.unlink(missing_ok=True)
        skipped.sort(key=lambda an: _candidate_rank(an[1]))
        return skipped[0][0]
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


def _unsplash_key() -> str | None:
    key = os.environ.get("UNSPLASH_ACCESS_KEY")
    if key:
        return key.strip()
    try:
        return UNSPLASH_KEY_FILE.read_text().strip() or None
    except OSError:
        return None


def load_vacation() -> dict | None:
    """Read the vacation config and compute the countdown. Returns None if no
    trip is configured, the file is malformed, or the trip is over (departed
    more than VACATION_TRIP_DAYS ago).

    Config: {"destination": "Tokyo, Japan", "date": "2026-09-01",
             "query": "Tokyo skyline" (optional image-search override)}"""
    try:
        raw = VACATION_FILE.read_text()
    except OSError:
        return None
    try:
        cfg = json.loads(raw)
    except (json.JSONDecodeError, ValueError):
        log("vacation.json is not valid JSON")
        return None
    dest = (cfg.get("destination") or "").strip()
    date_str = (cfg.get("date") or "").strip()
    if not dest or not date_str:
        return None
    try:
        trip = datetime.strptime(date_str, "%Y-%m-%d").date()
    except ValueError:
        log(f"vacation.json: bad date {date_str!r} (want YYYY-MM-DD)")
        return None
    days = (trip - datetime.now().date()).days
    if days < -VACATION_TRIP_DAYS:
        return None
    return {
        "destination": dest[:63],
        "days": days,
        "date_label": f"{trip.day} {trip.strftime('%b %Y')}",  # "1 Sep 2026"
        "query": (cfg.get("query") or dest).strip(),
    }


def _photo_tokens(photo: dict) -> set:
    """Word tokens from a photo's tags + descriptions, lowercased."""
    parts = [photo.get("description") or "", photo.get("alt_description") or ""]
    for t in (photo.get("tags") or []):
        if isinstance(t, dict):
            parts.append(t.get("title") or "")
    return set(re.split(r"[^a-z0-9]+", " ".join(parts).lower()))


def _pick_place_photo(results: list) -> dict | None:
    """Prefer landscape/city photos; skip anything tagged people or animals.
    Word-level matching. Falls back to top relevance if everything is filtered."""
    scored = []
    for p in results:
        toks = _photo_tokens(p)
        reject = bool(toks & UNSPLASH_REJECT)
        prefer = bool(toks & UNSPLASH_PREFER)
        scored.append((p, reject, prefer))
    preferred = [p for (p, rej, pref) in scored if not rej and pref]
    if preferred:
        return preferred[0]
    clean = [p for (p, rej, _) in scored if not rej]
    if clean:
        return clean[0]
    return results[0] if results else None   # fallback: nothing passed


async def _unsplash_image_url(query: str, key: str) -> str | None:
    """Resolve a square place photo URL via the Unsplash search API, biased
    toward landscapes/cityscapes and away from people/animals. Honours the
    download-tracking guideline."""
    headers = {"Authorization": f"Client-ID {key}",
               "Accept-Version": "v1"}
    # Search the plain destination (adding bias words to the query starves
    # non-city places like "Namibia" of results); the people/animal filtering
    # and landscape preference happen on the result set in _pick_place_photo.
    params = {"query": query, "orientation": "squarish",
              "per_page": "30", "content_filter": "high"}
    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            r = await http.get(UNSPLASH_SEARCH, headers=headers, params=params)
            if r.status_code != 200:
                log(f"Unsplash HTTP {r.status_code}: {r.text[:120]}")
                return None
            results = r.json().get("results") or []
            if not results:
                log(f"Unsplash: no photo for {query!r}")
                return None
            photo = _pick_place_photo(results)
            if photo is None:
                return None
            # Attribution guideline: ping the download endpoint (fire-and-forget).
            dl = (photo.get("links") or {}).get("download_location")
            if dl:
                try:
                    await http.get(dl, headers=headers)
                except httpx.HTTPError:
                    pass
            raw = (photo.get("urls") or {}).get("raw")
            if not raw:
                return None
            return raw + "&w=480&h=480&fit=crop&crop=entropy&fm=jpg&q=80"
    except httpx.HTTPError as e:
        log(f"Unsplash request failed: {e}")
        return None


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
        self.last_afk: bool | None = None
        self.attn_active = False
        self.attn_started_at = 0.0
        # Watchdog: bumped whenever the main loop makes progress. If it goes
        # stale the watchdog task force-disconnects to unwedge a hung await.
        self.last_progress: float = time.time()
        self.last_history_at: float = 0.0
        self.last_history_sig: str | None = None
        self.last_vacation_at: float = 0.0
        self.last_vacation_sig: str | None = None
        self.last_vacation_dest: str | None = None
        self.vacation_img_task: asyncio.Task | None = None

    def note_progress(self) -> None:
        self.last_progress = time.time()
        heartbeat()

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
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=BLE_WRITE_TIMEOUT,
            )
            log("Refresh subscription OK")
        except Exception as e:
            log(f"Refresh subscription unavailable: {type(e).__name__}: {e}")
        try:
            await asyncio.wait_for(
                self.client.start_notify(CMD_CHAR_UUID, self._on_cmd),
                timeout=BLE_WRITE_TIMEOUT,
            )
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

    async def maybe_send_vacation(self) -> None:
        """Push the vacation countdown as {"vac":{...}} and, when the
        destination changes, the Unsplash backdrop over the VACIMG char."""
        now = time.time()
        if self.last_vacation_at and now - self.last_vacation_at < VACATION_INTERVAL_S:
            return
        self.last_vacation_at = now
        vac = load_vacation()
        if vac is None:
            if self.last_vacation_sig != "inactive":
                if await self.write_payload({"vac": {"a": False}}):
                    self.last_vacation_sig = "inactive"
                    self.last_vacation_dest = None
            return
        sig = f"{vac['destination']}|{vac['date_label']}|{vac['days']}"
        if sig != self.last_vacation_sig:
            payload = {"vac": {"dst": vac["destination"], "dl": vac["date_label"],
                               "d": vac["days"], "a": True}}
            if await self.write_payload(payload):
                self.last_vacation_sig = sig
                log(f"Vacation: {vac['destination']} in {vac['days']}d")
        # Fetch + push the backdrop only when the destination changes.
        if vac["query"] != self.last_vacation_dest and (
                not self.vacation_img_task or self.vacation_img_task.done()):
            self.last_vacation_dest = vac["query"]
            self.vacation_img_task = asyncio.create_task(
                self._send_vacation_image(vac["query"]))

    async def _send_vacation_image(self, query: str) -> None:
        key = _unsplash_key()
        if not key:
            log("No Unsplash key — set UNSPLASH_ACCESS_KEY or write the key to "
                f"{UNSPLASH_KEY_FILE}; skipping vacation backdrop")
            self.last_vacation_dest = None   # retry once a key appears
            return
        url = await _unsplash_image_url(query, key)
        if not url:
            self.last_vacation_dest = None
            return
        rgb = await _fetch_backdrop_rgb565(url)
        if rgb is None:
            self.last_vacation_dest = None
            return
        h = _url_hash16(url)
        log(f"Sending vacation backdrop for {query!r} ({len(rgb)} bytes)")
        try:
            # Write-with-response so CoreBluetooth/NimBLE both pace properly —
            # write_no_response on macOS silently drops most chunks, leaving
            # the ESP-side buffer half-filled.
            for offset in range(0, len(rgb), BACKDROP_CHUNK_PAYLOAD):
                chunk = rgb[offset:offset + BACKDROP_CHUNK_PAYLOAD]
                header = struct.pack("<HII", h, offset, len(rgb))
                await self._ble_write(VACIMG_CHAR_UUID, header + chunk, response=True)
            log("Vacation backdrop sent")
        except (BleakError, asyncio.TimeoutError) as e:
            log(f"Vacation backdrop send failed: {e}")
            self.last_vacation_dest = None   # retry next cycle

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
        # CBErrorDomain 14 = the peer no longer has the bond this Mac still
        # holds (ESP NVS wiped, bonds cleared on-device, or a fresh chip). The
        # Mac keeps that identity around and keeps offering it in scans, so
        # blacklist it and let the next scan pick the live one.
        if "removed pairing information" in str(e):
            log("Bond mismatch: this Mac holds a pairing the device no longer "
                "has. If no other identity for it turns up, forget the device "
                "in System Settings -> Bluetooth and let it re-pair.")
            mark_bad_address(address)
            SAVED_ADDR_FILE.unlink(missing_ok=True)
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
                await safe_disconnect(client)
                return

    watchdog = asyncio.create_task(_watchdog())
    last_poll = 0.0
    last_tick_at = time.time()
    last_awake_at = awake_clock()
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            awake = awake_clock()
            session.note_progress()
            # Sleep / wake detection — the BLE link is almost always in a
            # stale state after wake, so we tear down the client and let the
            # outer loop reconnect from a clean slate. Compare wall time with
            # awake time rather than raw tick gaps: one iteration can
            # legitimately take 20-30s (album art over BLE), and treating that
            # as sleep tore down every fresh session in an endless loop.
            slept = (now - last_tick_at) - (awake - last_awake_at)
            if slept > SLEEP_GAP_S:
                # Keep the cached address: the CoreBluetooth peer UUID survives
                # sleep, and dropping it forces a full rescan — the slow, fragile
                # path. The 3-consecutive-failure rule in main() still
                # invalidates it if the peer really is gone.
                log(f"Sleep/wake: host slept {slept:.0f}s — tearing down BLE client")
                break
            last_tick_at = now
            last_awake_at = awake
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
            await session.maybe_send_token_history()
            await session.maybe_send_vacation()

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        # Cancel background tasks before tearing the link down so they don't
        # leak as "Task was destroyed but it is pending" against a dead client.
        # Every await here is bounded: this runs after the watchdog is gone, so
        # a hang would freeze the daemon for good (seen: disconnect() never
        # returning because CoreBluetooth skipped the didDisconnect callback).
        for task in (watchdog, session.vacation_img_task):
            if task and not task.done():
                task.cancel()
                try:
                    await asyncio.wait_for(task, timeout=DISCONNECT_TIMEOUT)
                except (asyncio.CancelledError, Exception):
                    pass
        await safe_disconnect(client)

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
    start_liveness_watchdog()

    backoff = 1
    consec_connect_failures = 0
    # Set after repeated failures so the next round goes through discovery
    # instead of hammering a known-good peer that has gone stale (macOS can
    # remint a peer identity, e.g. after a re-pair).
    skip_known_good = False
    while not stop_event.is_set():
        heartbeat()
        # Order matters: the scan-cached address first, then the peer that last
        # actually delivered data, then a fresh scan. The known-good peer beats
        # a scan because macOS hides peripherals it already holds a connection
        # to (ours is bonded as an HID keyboard), so the only thing a scan can
        # see may be a *different* nearby board.
        address = load_cached_address()
        if not address and not skip_known_good:
            address = load_last_good_address()
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
        if ok:
            save_last_good_address(address)
            skip_known_good = False
        if not ok:
            consec_connect_failures += 1
            # Keep the cached UUID across transient failures (e.g. laptop
            # wake-from-sleep) — only nuke it after several attempts, since
            # macOS can hold the peer in a stale state for a few seconds.
            if consec_connect_failures >= 3:
                log("Connect failed 3x, invalidating cached address "
                    "and forcing rediscovery")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
                skip_known_good = True
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
