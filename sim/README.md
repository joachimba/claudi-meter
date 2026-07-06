# Clawdmeter desktop simulator

Renders the firmware's LVGL UI into an SDL2 window so screens can be iterated
on without an ESP32.

## Build

```bash
brew install cmake sdl2
cd sim
cmake -B build -S .
cmake --build build -j
```

## Run

```bash
./build/clawdmeter-sim
```

## Controls

| Key       | Action                                         |
| --------- | ---------------------------------------------- |
| `1`..`5`  | Show Usage / Spotify / Calendar / Bluetooth / Splash |
| `SPACE`   | Cycle screens (Usage → Spotify → Calendar → Bluetooth) |
| `a`       | Cycle gradient album-art placeholder           |
| `p`       | Toggle Spotify play / paused                   |
| `r`       | Re-randomize fake usage / calendar numbers     |
| mouse     | Acts as touch input (click = tap)              |
| `q` / Esc | Quit                                           |

## What the sim covers

- Pixel-perfect UI render (same LVGL widgets, same fonts, same theme).
- Touch input (mouse click).
- Splash, Usage, Spotify, Calendar, Bluetooth screens.
- Album art via synthetic gradient (real PNGs would need stb_image hooked in).

## What it doesn't cover

- Real BLE link (stubbed; ble state is forced to `CONNECTED`).
- Power / IMU / rotation (stubbed: 82 % battery, rotation 0).
- IMU-driven CPU pixel-remap rotation logic.
- Daemon Spotify / calendar fetching (sim feeds fake data directly).
