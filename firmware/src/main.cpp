#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "settings.h"
#include "pomodoro.h"
#include "oled_protect.h"
#include "usage_rate.h"

// Physical buttons (global, screen-independent):
//   BTN_BACK   (GPIO 0)  — left,  send Space (Claude Code voice mode push-to-talk)
//   BTN_FWD    (GPIO 18) — right, send Shift+Tab (Claude Code mode toggle)
//   AXP PWR    (PMU)     — middle, cycle screens; on splash, cycle animations
#define BTN_BACK 0
#define BTN_FWD  18

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;

static UsageData usage = {};
static SpotifyData spotify = {};
static CalendarData calendar = {};
static PhotoData photo = {};
static BTAccessoriesData bt_acc = {};
static HistoryData history = {};

// ---- Touch interrupt + shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read() {
    if (!touch_data_ready) return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
// rot_buf for strip rotation — max size is 480×480 (full invalidation case)
// but typical partial strips are much smaller
static uint16_t *rot_buf = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// Rotate a w×h strip and compute destination coordinates on the 480×480 display.
// src pixels are in row-major order for the rectangle (sx, sy, w, h).
// Output goes to rot_buf in row-major order for the destination rectangle.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    const int S = LCD_WIDTH;  // 480

    switch (r) {
    case 1: { // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(h-1-y, x)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    }
    case 2: { // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    }
    case 3: { // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(y, w-1-x)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    }
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

// LVGL flush callback — rotates partial strips and writes to display
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
    uint8_t r = imu_get_rotation();

    if (r == 0) {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    } else {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
    }
    lv_display_flush_ready(disp);
}

// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData / SpotifyData / CalendarData.
// Daemon sends one merged payload; missing sections leave their valid flag
// unchanged so a partial update doesn't blank existing UI state.
static bool parse_json(const char* json, UsageData* u, SpotifyData* sp,
                       CalendarData* cal, PhotoData* pho, BTAccessoriesData* bta,
                       HistoryData* hist) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    if (doc["s"].is<float>() || doc["s"].is<int>()) {
        u->session_pct = doc["s"] | 0.0f;
        u->session_reset_mins = doc["sr"] | -1;
        u->weekly_pct = doc["w"] | 0.0f;
        u->weekly_reset_mins = doc["wr"] | -1;
        strlcpy(u->status, doc["st"] | "unknown", sizeof(u->status));
        u->ok = doc["ok"] | false;
        u->valid = true;
    }

    if (doc["spt"].is<const char*>() || doc["spa"].is<const char*>() || doc["spp"].is<bool>()) {
        strlcpy(sp->track, doc["spt"] | "", sizeof(sp->track));
        strlcpy(sp->artist, doc["spa"] | "", sizeof(sp->artist));
        sp->playing = doc["spp"] | false;
        sp->valid = true;
    }

    if (doc["spc"].is<int>() || doc["spc"].is<unsigned int>() || doc["spc"].is<long>() ||
        doc["spc"].is<unsigned long>()) {
        sp->bg_color = (uint32_t)(doc["spc"].as<long>());
        sp->has_color = true;
        sp->valid = true;
    }

    if (doc["cat"].is<const char*>() || doc["cam"].is<int>()) {
        strlcpy(cal->title, doc["cat"] | "", sizeof(cal->title));
        cal->mins_until = doc["cam"] | -1;
        cal->valid = true;
    }

    if (doc["pht"].is<const char*>()) {
        strlcpy(pho->caption, doc["pht"] | "", sizeof(pho->caption));
        pho->valid = true;
    }

    if (doc["afk"].is<bool>()) {
        ui_set_afk(doc["afk"].as<bool>());
    }

    if (doc["attn"].is<bool>()) {
        ui_set_attention(doc["attn"].as<bool>());
    }

    if (doc["bta"].is<JsonArrayConst>()) {
        JsonArrayConst arr = doc["bta"].as<JsonArrayConst>();
        bta->count = 0;
        for (JsonVariantConst v : arr) {
            if (bta->count >= BT_ACC_MAX) break;
            BTAccessory& dst = bta->items[bta->count];
            strlcpy(dst.name, v["n"] | "", sizeof(dst.name));
            dst.battery_pct = v["b"] | -1;
            dst.charging    = v["c"] | false;
            bta->count++;
        }
        bta->valid = true;
    }

    if (doc["tk"].is<JsonArrayConst>()) {
        JsonArrayConst arr = doc["tk"].as<JsonArrayConst>();
        hist->count = 0;
        for (JsonVariantConst v : arr) {
            if (hist->count >= HISTORY_MAX_DAYS) break;
            hist->vals[hist->count++] = (uint16_t)(v.as<unsigned int>());
        }
        hist->valid = true;
    }

    if (doc["ck"].is<JsonArrayConst>()) {
        JsonArrayConst arr = doc["ck"].as<JsonArrayConst>();
        uint8_t i = 0;
        for (JsonVariantConst v : arr) {
            if (i >= HISTORY_MAX_DAYS) break;
            hist->cost_cents[i++] = (uint32_t)(v.as<unsigned long>());
        }
        hist->cost_valid = true;
    }

    return true;
}

// Serial command buffer
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init I2C (shared by touch + PMU)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // Init PMU
    power_init();

    // Init IMU (accelerometer for auto-rotation)
    imu_init();

    // Init touch
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
    } else {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate PSRAM-backed partial render buffers
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    // rot_buf needs to hold the largest possible strip after rotation
    // A 480×40 strip rotated 90° becomes 40×480, same pixel count
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // CO5300 even-alignment rounder
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init settings (NVS-backed) before UI so initial widget state matches
    settings_init();
    pomodoro_init();
    oled_protect_init();

    // Init BLE data channel
    ble_init();

    // Physical buttons: back (GPIO 0) and forward (GPIO 18)
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_FWD,  INPUT_PULLUP);

    // Build dashboard
    ui_init();

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Brightness ramp state for rotation transition
// On rotation change we blank the panel, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125ms so the
// transition reads as deliberate instead of as a glitch.
static void handle_rotation_change(void) {
    static uint8_t last_rotation = 0;
    static uint8_t  ramp_step = 0;  // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation) {
        gfx->setBrightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    gfx->setBrightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();
    pomodoro_tick();
    oled_protect_tick();

    // Three-button input (global, screen-independent):
    //   LEFT  (GPIO 0)  → Space (voice-mode push-to-talk; press & release tracked)
    //   RIGHT (GPIO 18) → Shift+Tab (Claude Code mode toggle)
    //   PWR   (AXP)     → cycle screens; on splash, cycle animations
    {
        static bool back_was = false, fwd_was = false;
        bool back_now = (digitalRead(BTN_BACK) == LOW);
        bool fwd_now  = (digitalRead(BTN_FWD)  == LOW);

        if (back_now != back_was) {
            if (back_now) ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            else          ble_keyboard_release();
            back_was = back_now;
            oled_protect_note_activity();
        }
        if (fwd_now != fwd_was) {
            if (fwd_now) ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
            else         ble_keyboard_release();
            fwd_was = fwd_now;
            oled_protect_note_activity();
        }

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
            oled_protect_note_activity();
        }
        if (touch_pressed) oled_protect_note_activity();
    }

    // Auto-rotate screens (skips Bluetooth + Splash). When the user enables
    // this in Settings the dashboard cycles Usage → Spotify → Calendar →
    // Settings → Usage at the configured interval.
    {
        static uint32_t last_rotate_ms = 0;
        if (settings_auto_rotate()) {
            uint32_t now = millis();
            uint32_t interval = (uint32_t)settings_rotate_interval_sec() * 1000UL;
            if (last_rotate_ms == 0) last_rotate_ms = now;
            screen_t cs = ui_get_current_screen();
            if (cs != SCREEN_SPLASH && cs != SCREEN_BLUETOOTH &&
                cs != SCREEN_SETTINGS &&
                now - last_rotate_ms >= interval) {
                ui_cycle_screen_auto();
                last_rotate_ms = now;
            }
        } else {
            last_rotate_ms = 0;
        }
    }

    handle_rotation_change();

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Firmware-side stale-data watchdog: when the host laptop sleeps the
    // daemon is suspended too, so it can't push an `afk:true` flag itself.
    // If we haven't seen any JSON for a while, flip the AFK overlay on
    // locally; clears the next time data arrives.
    static uint32_t last_json_ms = 0;
    {
        const uint32_t STALE_MS = 120UL * 1000UL;
        if (last_json_ms != 0 && (millis() - last_json_ms) > STALE_MS) {
            ui_set_afk(true);
        }
    }

    // Process incoming BLE data
    if (ble_has_data()) {
        oled_protect_note_activity();
        if (parse_json(ble_get_data(), &usage, &spotify, &calendar, &photo, &bt_acc, &history)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            last_json_ms = millis();
            // Daemon is alive again — assume not-AFK unless the daemon
            // explicitly told us otherwise via the `afk` JSON key (handled
            // inside parse_json).
            ui_set_afk(false);
            ui_update(&usage);
            ui_update_spotify(&spotify);
            ui_update_calendar(&calendar);
            ui_update_photo_caption(&photo);
            ui_update_bt_accessories(&bt_acc);
            ui_update_history(&history);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    // Album art arrives in chunks on a separate GATT char; render when complete
    if (ble_image_ready()) {
        ui_update_spotify_art(ble_image_data(), BLE_ART_W, BLE_ART_H);
        ble_image_consume();
    }

    // Ambient photo (full screen) reassembled on its own GATT char.
    if (ble_photo_ready()) {
        ui_update_photo_image(ble_photo_data());
        ble_photo_consume();
    }

    // Hide the canvas while a new photo is mid-transfer so the user doesn't
    // see half-old / half-new pixels rendered together.
    {
        static bool last_xfer = false;
        bool xfer = ble_photo_transferring();
        if (xfer != last_xfer) {
            last_xfer = xfer;
            extern void ui_set_photo_loading(bool);
            ui_set_photo_loading(xfer);
        }
    }

    delay(5);
}
