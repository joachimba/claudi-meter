#include "ui.h"
#include "splash.h"
#include "splash_animations.h"
#include "settings.h"
#include "pomodoro.h"
#include <lvgl.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "logo.h"
#include "icons.h"
#include "display_cfg.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_mono_32);

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Layout constants for 480x480 (scaled for 2.16" high-DPI + rounded corners) ----
#define SCR_W         480
#define SCR_H         480
#define MARGIN        20    // wider margin for rounded display corners
#define TITLE_Y       30
#define CONTENT_Y     100
#define CONTENT_W     (SCR_W - 2 * MARGIN)   // 440

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;

// ---- Spotify screen widgets ----
static lv_obj_t* spotify_container;
static lv_obj_t* spotify_panel;
static lv_obj_t* lbl_spotify_state;
static lv_obj_t* lbl_spotify_track;
static lv_obj_t* lbl_spotify_artist;
static lv_obj_t* img_spotify_art;
static lv_image_dsc_t spotify_art_dsc;
static bool spotify_art_seen = false;

// Tiny pixel-art Clawd dancer below the Spotify panel — recycles the splash
// engine's 20×20 sprite catalog at a smaller cell size so it sits in the
// 120 px of space under the player card.
#define SP_DANCE_GRID 20
#define SP_DANCE_CELL 5
#define SP_DANCE_W    (SP_DANCE_GRID * SP_DANCE_CELL)
#define SP_DANCE_H    (SP_DANCE_GRID * SP_DANCE_CELL)
static lv_obj_t* spotify_dance_canvas = nullptr;
static uint16_t* spotify_dance_buf    = nullptr;
static int8_t    spotify_dance_idx    = -1;
static uint16_t  spotify_dance_frame  = 0;
static uint32_t  spotify_dance_last_ms = 0;

// Mix two channels with weight `w` (0..255) of `a`. Used to darken a vibrant
// dominant album-art colour down to a panel-friendly tint.
static inline uint8_t mix_u8(uint8_t a, uint8_t b, uint8_t w) {
    return (uint8_t)(((int)a * w + (int)b * (255 - w)) / 255);
}
static lv_color_t darken_rgb24(uint32_t rgb, uint8_t mix_pct) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8)  & 0xFF;
    uint8_t b =  rgb        & 0xFF;
    uint8_t w = (uint8_t)((int)mix_pct * 255 / 100);
    return lv_color_make(mix_u8(r, 0, w), mix_u8(g, 0, w), mix_u8(b, 0, w));
}

// ---- Calendar screen widgets ----
static lv_obj_t* calendar_container;
static lv_obj_t* lbl_calendar_state;
static lv_obj_t* lbl_calendar_event;
static lv_obj_t* lbl_calendar_when;

// ---- Settings screen widgets ----
static lv_obj_t* settings_container;
static lv_obj_t* sw_auto_rotate;

// ---- Pomodoro screen widgets ----
static lv_obj_t* pomodoro_container;
static lv_obj_t* pomodoro_arc;
static lv_obj_t* lbl_pomodoro_state;
static lv_obj_t* lbl_pomodoro_time;
static lv_obj_t* lbl_pomodoro_cycles;
static lv_obj_t* lbl_pomodoro_hint;

// ---- Photo screen widgets ----
static lv_obj_t* photo_container;
static lv_obj_t* photo_canvas;
static lv_obj_t* photo_caption_box;
static lv_obj_t* lbl_photo_caption;
static bool photo_have_image = false;

// ---- Bluetooth Accessories screen widgets ----
static lv_obj_t* btacc_container;
static lv_obj_t* btacc_list_parent;
static lv_obj_t* btacc_empty_label;

// ---- Usage History (daily token graph) screen ----
#define HG_W 440          // graph canvas width
#define HG_H 200          // graph canvas height
#define HG_PAD_TOP 16     // keep the peak off the top edge
#define HG_BOTTOM_INSET 22 // clear the rounded display corners below the graph
static lv_obj_t* history_container = nullptr;
static lv_obj_t* lbl_hist_caption  = nullptr;   // "TOKENS / DAY"
static lv_obj_t* lbl_hist_total    = nullptr;   // 30-day equivalent-cost total (top-right)
static lv_obj_t* lbl_hist_value    = nullptr;   // big headline (today)
static lv_obj_t* lbl_hist_delta    = nullptr;   // vs yesterday + today's cost
static lv_obj_t* history_canvas    = nullptr;
static uint16_t* history_buf       = nullptr;
static HistoryData history_data    = {};        // last payload, kept for redraw

// ---- Vacation countdown screen (Polarsteps-style: photo + flip-clock tiles) ----
#define VAC_TILE_MAX 3          // up to 3 digits (e.g. 365)
#define VAC_TILE_W   74
#define VAC_TILE_H   98
static lv_obj_t* vacation_container = nullptr;
static lv_obj_t* vac_canvas        = nullptr;   // full-screen backdrop (BLE vacimg buffer)
static lv_obj_t* vac_overlay       = nullptr;   // full-screen dark tint for legibility
static lv_obj_t* vac_tiles         = nullptr;   // centered row of digit cards
static lv_obj_t* vac_tile[VAC_TILE_MAX]     = {nullptr};
static lv_obj_t* vac_tile_lbl[VAC_TILE_MAX] = {nullptr};
static lv_obj_t* lbl_vac_caption   = nullptr;   // "nights until"
static lv_obj_t* lbl_vac_dest      = nullptr;   // destination name
static lv_obj_t* lbl_vac_empty     = nullptr;   // "No trip set" placeholder
static bool      vac_have_image    = false;

// ---- AFK overlay (sits above every screen when the host reports idle) ----
#define AFK_ANIM_GRID 20
#define AFK_ANIM_CELL 8
#define AFK_ANIM_W (AFK_ANIM_GRID * AFK_ANIM_CELL)
#define AFK_ANIM_H (AFK_ANIM_GRID * AFK_ANIM_CELL)
static lv_obj_t* afk_layer = nullptr;
static lv_obj_t* afk_canvas = nullptr;
static uint16_t* afk_anim_buf = nullptr;
static int8_t    afk_anim_idx = -1;
static uint16_t  afk_anim_frame = 0;
static uint32_t  afk_anim_last_ms = 0;
static bool      afk_is_on = false;
// Manual escape hatch: a tap or button press hides the overlay and mutes the
// firmware-side stale-data watchdog for a while, so a daemon that can't
// reconnect (dead BLE link, unpaired host) can't trap the UI behind it.
static uint32_t  afk_dismissed_ms = 0;
#define AFK_SUPPRESS_MS (10UL * 60UL * 1000UL)

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

// Per-frame hold time. Modeled on Claude Code's spinner (Cavalry triangle
// oscillator, range 0..5, period 5s) — turn-around frames (0 and 5) appear
// once per cycle, middle frames twice, so 0/5 read as held longer.
static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Bubble click events up to the screen / usage_container so a tap anywhere
    // on the panel fires the global click handler.
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

// RGB565A8: planar — w*h RGB565 pixels followed by w*h alpha bytes.
// Stride is RGB565-only (w*2); LVGL infers alpha plane location from header.
static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

// ---- Battery icon initialization ----
static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen (480x480) ========

#define PANEL_H     150
#define PANEL_GAP   16

// One Session/Weekly panel: big % label, pill on the right, bar, reset label.
// Pill y=1: symmetric inside the panel — panel-outer-top → pill-top equals
// pill-bottom → bar-top (pill height 42 + panel pad_top 12 + bar y=56).
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, MARGIN, y, CONTENT_W, PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, 56, CONTENT_W - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 94);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, SCR_W, SCR_H);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    make_usage_panel(usage_container, CONTENT_Y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Usage History Screen — smooth daily token graph ========

// Pack r,g,b into the RGB565 value LVGL stores in a canvas buffer.
static inline uint16_t hg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return lv_color_to_u16(lv_color_make(r, g, b));
}

// Anthropic terra-cotta (THEME_ACCENT 0xd97757) as components, for scaling.
#define HG_ACC_R 0xD9
#define HG_ACC_G 0x77
#define HG_ACC_B 0x57

// Format a token count into a compact headline: 47.3M / 812k / 137.
static void hg_format_tokens(uint32_t tok, char* buf, size_t n) {
    if (tok >= 1000000000UL)      snprintf(buf, n, "%.1fB", tok / 1e9);
    else if (tok >= 1000000UL)    snprintf(buf, n, "%.1fM", tok / 1e6);
    else if (tok >= 1000UL)       snprintf(buf, n, "%.0fk", tok / 1e3);
    else                          snprintf(buf, n, "%lu", (unsigned long)tok);
}

// Format a cent amount into a compact dollar string: $2.2K / $340 / $5.20.
static void hg_format_usd(uint32_t cents, char* buf, size_t n) {
    if (cents >= 100000UL)   snprintf(buf, n, "$%.1fK", cents / 100000.0);  // >= $1000
    else if (cents >= 1000UL) snprintf(buf, n, "$%lu", (unsigned long)(cents / 100)); // >= $10
    else                     snprintf(buf, n, "$%.2f", cents / 100.0);
}

// Redraw the graph from history_data into history_buf.
static void render_history_graph(void) {
    if (!history_buf) return;
    const int W = HG_W, H = HG_H;
    // Clear to black.
    for (int i = 0; i < W * H; i++) history_buf[i] = 0x0000;

    int n = history_data.count;
    if (!history_data.valid || n < 2) {
        if (history_canvas) lv_obj_invalidate(history_canvas);
        return;
    }
    if (n > HISTORY_MAX_DAYS) n = HISTORY_MAX_DAYS;

    // Peak for vertical scaling (baseline is 0 tokens).
    uint16_t vmax = 1;
    for (int i = 0; i < n; i++) if (history_data.vals[i] > vmax) vmax = history_data.vals[i];

    const int plotH = H - 1 - HG_PAD_TOP;
    // Screen-Y for each data point (top of the curve).
    static float yv[HISTORY_MAX_DAYS];
    for (int i = 0; i < n; i++) {
        float frac = (float)history_data.vals[i] / (float)vmax;   // 0..1
        yv[i] = (float)(H - 1) - frac * (float)plotH;
    }

    // Faint dashed baseline gridlines (25/50/75%).
    uint16_t grid = hg_rgb(40, 40, 38);
    for (int g = 1; g <= 3; g++) {
        int gy = (H - 1) - (plotH * g) / 4;
        if (gy < 0 || gy >= H) continue;
        for (int x = 0; x < W; x += 10)
            for (int dx = 0; dx < 5 && x + dx < W; dx++)
                history_buf[gy * W + x + dx] = grid;
    }

    // For each pixel column, evaluate the Catmull-Rom curve through the points.
    static int colTop[HG_W];
    for (int px = 0; px < W; px++) {
        float f = (float)px / (float)(W - 1) * (float)(n - 1);
        int i = (int)f;
        if (i > n - 2) i = n - 2;
        float t = f - i;
        float y0 = yv[i > 0 ? i - 1 : 0];
        float y1 = yv[i];
        float y2 = yv[i + 1];
        float y3 = yv[i < n - 2 ? i + 2 : n - 1];
        float y = 0.5f * ((2.0f * y1) + (-y0 + y2) * t
                          + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t * t
                          + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t * t * t);
        int iy = (int)(y + 0.5f);
        if (iy < HG_PAD_TOP) iy = HG_PAD_TOP;
        if (iy > H - 1) iy = H - 1;
        colTop[px] = iy;

        // Gradient area fill: vivid terra-cotta at the curve fading to black.
        for (int yy = iy; yy < H; yy++) {
            float depth = (H - 1 - iy) > 0 ? (float)(yy - iy) / (float)(H - 1 - iy) : 0.0f;
            float t2 = 1.0f - depth;
            float k = 0.06f + 0.80f * t2 * t2;   // ~0.86 near line -> 0.06 at floor
            history_buf[yy * W + px] = hg_rgb((uint8_t)(HG_ACC_R * k),
                                              (uint8_t)(HG_ACC_G * k),
                                              (uint8_t)(HG_ACC_B * k));
        }
    }

    // Stroke the curve line on top (3px, brightened terra-cotta so it pops).
    uint16_t line = hg_rgb(0xFF, 0xA6, 0x88);
    for (int px = 0; px < W; px++) {
        for (int d = -2; d <= 0; d++) {
            int yy = colTop[px] + d;
            if (yy >= 0 && yy < H) history_buf[yy * W + px] = line;
        }
    }

    // End dot at the most recent day (anchor the right edge). Inset a couple
    // px so the full disc stays on-canvas.
    int ex = W - 6, ey = colTop[W - 1];
    uint16_t glow = hg_rgb(0xFF, 0xF2, 0xEC);   // near-white core
    for (int dy = -8; dy <= 8; dy++)
        for (int dx = -8; dx <= 8; dx++) {
            int x = ex + dx, y = ey + dy;
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            int r2 = dx * dx + dy * dy;
            if (r2 <= 49) history_buf[y * W + x] = (r2 <= 12) ? glow : line;
        }

    if (history_canvas) lv_obj_invalidate(history_canvas);
}

static void init_history_screen(lv_obj_t* scr) {
    history_container = lv_obj_create(scr);
    lv_obj_set_size(history_container, SCR_W, SCR_H);
    lv_obj_set_pos(history_container, 0, 0);
    lv_obj_set_style_bg_opa(history_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(history_container, 0, 0);
    lv_obj_set_style_pad_all(history_container, 0, 0);
    lv_obj_clear_flag(history_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(history_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Header block sits below the persistent top-left logo (y clears 100px).
    lbl_hist_caption = lv_label_create(history_container);
    lv_label_set_text(lbl_hist_caption, "TOKENS / DAY");
    lv_obj_set_style_text_font(lbl_hist_caption, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_hist_caption, COL_DIM, 0);
    lv_obj_set_style_text_letter_space(lbl_hist_caption, 3, 0);
    lv_obj_set_pos(lbl_hist_caption, MARGIN, 104);

    // 30-day equivalent-cost total, right-aligned on the caption row.
    lbl_hist_total = lv_label_create(history_container);
    lv_label_set_text(lbl_hist_total, "");
    lv_obj_set_style_text_font(lbl_hist_total, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_hist_total, COL_DIM, 0);
    lv_obj_set_style_text_letter_space(lbl_hist_total, 2, 0);
    lv_obj_align(lbl_hist_total, LV_ALIGN_TOP_RIGHT, -MARGIN, 106);

    lbl_hist_value = lv_label_create(history_container);
    lv_label_set_text(lbl_hist_value, "--");
    lv_obj_set_style_text_font(lbl_hist_value, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_hist_value, COL_TEXT, 0);
    lv_obj_set_pos(lbl_hist_value, MARGIN, 128);

    lbl_hist_delta = lv_label_create(history_container);
    lv_label_set_text(lbl_hist_delta, "");
    lv_obj_set_style_text_font(lbl_hist_delta, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_hist_delta, COL_DIM, 0);
    lv_obj_set_pos(lbl_hist_delta, MARGIN, 196);

    // Full-width graph canvas along the bottom.
    history_buf = (uint16_t*)heap_caps_malloc(HG_W * HG_H * 2, MALLOC_CAP_SPIRAM);
    if (history_buf) {
        memset(history_buf, 0, HG_W * HG_H * 2);
        history_canvas = lv_canvas_create(history_container);
        lv_canvas_set_buffer(history_canvas, history_buf, HG_W, HG_H,
                             LV_COLOR_FORMAT_RGB565);
        // Inset from the bottom so the curve baseline clears the rounded corners.
        lv_obj_set_pos(history_canvas, MARGIN, SCR_H - HG_H - HG_BOTTOM_INSET);
    }

    lv_obj_add_flag(history_container, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_history(const HistoryData* data) {
    if (!data || !data->valid || !lbl_hist_value) return;
    history_data = *data;

    int n = history_data.count;
    if (n < 1) return;

    uint32_t today = (uint32_t)history_data.vals[n - 1] * 100000UL;
    char buf[24];
    hg_format_tokens(today, buf, sizeof(buf));
    lv_label_set_text(lbl_hist_value, buf);

    // Delta vs yesterday, plus today's equivalent cost when available.
    char db[64], tb[24];
    long delta = 0;
    if (n >= 2) {
        long yest = (long)history_data.vals[n - 2] * 100000L;
        delta = (long)today - yest;
        hg_format_tokens((uint32_t)(delta < 0 ? -delta : delta), tb, sizeof(tb));
        snprintf(db, sizeof(db), "%s%s", delta >= 0 ? "+" : "-", tb);
    } else {
        db[0] = '\0';
    }
    if (history_data.cost_valid) {
        char cb[24];
        hg_format_usd(history_data.cost_cents[n - 1], cb, sizeof(cb));
        size_t len = strlen(db);
        // ASCII "~" for approx — the font subset lacks ≈ and · glyphs.
        snprintf(db + len, sizeof(db) - len, "%s~%s today",
                 len ? "   " : "", cb);
    }
    lv_label_set_text(lbl_hist_delta, db);
    lv_obj_set_style_text_color(lbl_hist_delta,
                                delta >= 0 ? COL_GREEN : COL_RED, 0);

    // 30-day equivalent-cost total, top-right.
    if (history_data.cost_valid) {
        uint32_t total = 0;
        for (int i = 0; i < n; i++) total += history_data.cost_cents[i];
        char tot[24], cb[20];
        hg_format_usd(total, cb, sizeof(cb));
        snprintf(tot, sizeof(tot), "%s / 30D", cb);
        lv_label_set_text(lbl_hist_total, tot);
    }

    render_history_graph();
}

// ======== Vacation Countdown Screen — photo backdrop + big countdown ========

static void init_vacation_screen(lv_obj_t* scr) {
    vacation_container = lv_obj_create(scr);
    lv_obj_set_size(vacation_container, SCR_W, SCR_H);
    lv_obj_set_pos(vacation_container, 0, 0);
    lv_obj_set_style_bg_color(vacation_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(vacation_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vacation_container, 0, 0);
    lv_obj_set_style_pad_all(vacation_container, 0, 0);
    lv_obj_clear_flag(vacation_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(vacation_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Full-screen RGB565 backdrop, backed by the BLE vacation-image buffer.
    vac_canvas = lv_canvas_create(vacation_container);
    lv_canvas_set_buffer(vac_canvas, (void*)ble_vacimg_data(),
                         BLE_PHOTO_W, BLE_PHOTO_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(vac_canvas, 0, 0);
    lv_obj_set_style_pad_all(vac_canvas, 0, 0);
    lv_obj_set_style_border_width(vac_canvas, 0, 0);
    lv_obj_clear_flag(vac_canvas, LV_OBJ_FLAG_SCROLLABLE);

    // Whole-photo dark tint so white text reads anywhere over it.
    vac_overlay = lv_obj_create(vacation_container);
    lv_obj_set_size(vac_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(vac_overlay, 0, 0);
    lv_obj_set_style_bg_color(vac_overlay, lv_color_hex(0x0a1420), 0);
    lv_obj_set_style_bg_opa(vac_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(vac_overlay, 0, 0);
    lv_obj_set_style_radius(vac_overlay, 0, 0);
    lv_obj_clear_flag(vac_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Row of flip-clock digit cards, centered.
    const int tiles_y = 150;
    vac_tiles = lv_obj_create(vacation_container);
    lv_obj_set_size(vac_tiles, SCR_W, VAC_TILE_H);
    lv_obj_set_pos(vac_tiles, 0, tiles_y);
    lv_obj_set_style_bg_opa(vac_tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vac_tiles, 0, 0);
    lv_obj_set_style_pad_all(vac_tiles, 0, 0);
    lv_obj_set_style_pad_column(vac_tiles, 12, 0);
    lv_obj_clear_flag(vac_tiles, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(vac_tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vac_tiles, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < VAC_TILE_MAX; i++) {
        lv_obj_t* t = lv_obj_create(vac_tiles);
        lv_obj_set_size(t, VAC_TILE_W, VAC_TILE_H);
        lv_obj_set_style_bg_color(t, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_shadow_width(t, 12, 0);
        lv_obj_set_style_shadow_opa(t, LV_OPA_40, 0);
        lv_obj_set_style_shadow_color(t, lv_color_hex(0x000000), 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* d = lv_label_create(t);
        lv_label_set_text(d, "0");
        lv_obj_set_style_text_font(d, &font_styrene_48, 0);
        lv_obj_set_style_text_color(d, lv_color_hex(0x14181f), 0);
        lv_obj_center(d);

        // Split-flap seam across the middle of the card.
        lv_obj_t* seam = lv_obj_create(t);
        lv_obj_set_size(seam, VAC_TILE_W, 2);
        lv_obj_set_style_bg_color(seam, lv_color_hex(0xc9ccd2), 0);
        lv_obj_set_style_bg_opa(seam, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seam, 0, 0);
        lv_obj_set_style_radius(seam, 0, 0);
        lv_obj_clear_flag(seam, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(seam);

        vac_tile[i] = t;
        vac_tile_lbl[i] = d;
    }

    // "nights until" caption, centered under the cards.
    lbl_vac_caption = lv_label_create(vacation_container);
    lv_label_set_text(lbl_vac_caption, "nights until");
    lv_obj_set_style_text_font(lbl_vac_caption, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_vac_caption, lv_color_hex(0xd7dbe0), 0);
    lv_obj_set_style_text_letter_space(lbl_vac_caption, 2, 0);
    lv_obj_set_style_text_align(lbl_vac_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_vac_caption, SCR_W);
    lv_obj_set_pos(lbl_vac_caption, 0, tiles_y + VAC_TILE_H + 14);

    // Destination — big, centered, anchored near the bottom edge.
    lbl_vac_dest = lv_label_create(vacation_container);
    lv_label_set_text(lbl_vac_dest, "");
    lv_obj_set_style_text_font(lbl_vac_dest, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_vac_dest, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(lbl_vac_dest, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_vac_dest, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_vac_dest, CONTENT_W);
    lv_obj_align(lbl_vac_dest, LV_ALIGN_BOTTOM_MID, 0, -42);

    // Placeholder shown when no trip is configured.
    lbl_vac_empty = lv_label_create(vacation_container);
    lv_label_set_text(lbl_vac_empty, "No trip set\nAdd one from the Mac menu");
    lv_obj_set_style_text_font(lbl_vac_empty, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_vac_empty, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_vac_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_vac_empty, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(lbl_vac_empty, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(vacation_container, LV_OBJ_FLAG_HIDDEN);
}

// Toggle the countdown widgets vs the "no trip" placeholder.
static void vac_show_countdown(bool on) {
    lv_obj_t* group[] = {vac_tiles, lbl_vac_caption, lbl_vac_dest};
    for (lv_obj_t* o : group) {
        if (!o) continue;
        if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_vac_empty) {
        if (on) lv_obj_add_flag(lbl_vac_empty, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(lbl_vac_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_vacation(const VacationData* data) {
    if (!data || !data->valid || !vac_tiles) return;

    if (!data->active) {
        vac_show_countdown(false);
        return;
    }
    vac_show_countdown(true);

    // Number shown in the cards (clamped 0..999) and its caption.
    int disp = data->days;
    const char* cap;
    if (data->days > 1)       cap = "nights until";
    else if (data->days == 1) cap = "night until";
    else if (data->days == 0) { cap = "departure day"; disp = 0; }
    else                      { cap = "under way";     disp = 0; }
    if (disp < 0)   disp = 0;
    if (disp > 999) disp = 999;

    char num[8];
    snprintf(num, sizeof(num), "%d", disp);
    int len = (int)strlen(num);

    // One card per digit, centered; hide unused cards (flex skips them).
    for (int i = 0; i < VAC_TILE_MAX; i++) {
        if (i < len) {
            char c[2] = { num[i], '\0' };
            lv_label_set_text(vac_tile_lbl[i], c);
            lv_obj_clear_flag(vac_tile[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(vac_tile[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_label_set_text(lbl_vac_caption, cap);
    lv_label_set_text(lbl_vac_dest, data->destination);
}

void ui_update_vacation_image(const uint8_t* rgb565) {
    if (!vac_canvas || !rgb565) return;
    vac_have_image = true;
    lv_obj_invalidate(vac_canvas);
}

// ======== Bluetooth Screen (480x480) ========

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, SCR_W, SCR_H);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Info panel (taller for 480x480)
    lv_obj_t* p_info = make_panel(ble_container, MARGIN, CONTENT_Y, CONTENT_W, 160);

    // Bluetooth icon + status row
    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 56, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    // Reset Bluetooth tap zone with trash icon
    int reset_y = CONTENT_Y + 160 + 16;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, MARGIN, reset_y);
    lv_obj_set_size(reset_zone, CONTENT_W, 110);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    // Start hidden
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Spotify Screen (480x480) ========

static void init_spotify_screen(lv_obj_t* scr) {
    spotify_container = lv_obj_create(scr);
    lv_obj_set_size(spotify_container, SCR_W, SCR_H);
    lv_obj_set_pos(spotify_container, 0, 0);
    lv_obj_set_style_bg_opa(spotify_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spotify_container, 0, 0);
    lv_obj_set_style_pad_all(spotify_container, 0, 0);
    lv_obj_clear_flag(spotify_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(spotify_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(spotify_container);
    lv_label_set_text(title, "Spotify");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    spotify_panel = make_panel(spotify_container, MARGIN, CONTENT_Y, CONTENT_W, 260);
    lv_obj_t* panel = spotify_panel;

    img_spotify_art = lv_image_create(panel);
    lv_obj_set_size(img_spotify_art, BLE_ART_W, BLE_ART_H);
    lv_obj_set_pos(img_spotify_art, 0, 8);
    lv_obj_set_style_radius(img_spotify_art, 6, 0);
    lv_obj_set_style_clip_corner(img_spotify_art, true, 0);
    lv_obj_add_flag(img_spotify_art, LV_OBJ_FLAG_HIDDEN);

    const int text_x = BLE_ART_W + 16;
    const int text_w = CONTENT_W - 32 - text_x;

    // "NOW PLAYING" / "PAUSED" mini-pill above the title for a music-app look.
    lbl_spotify_state = lv_label_create(panel);
    lv_label_set_text(lbl_spotify_state, "IDLE");
    lv_obj_set_style_text_font(lbl_spotify_state, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_spotify_state, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl_spotify_state, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl_spotify_state, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_spotify_state, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl_spotify_state, 12, 0);
    lv_obj_set_style_pad_right(lbl_spotify_state, 12, 0);
    lv_obj_set_style_pad_top(lbl_spotify_state, 4, 0);
    lv_obj_set_style_pad_bottom(lbl_spotify_state, 4, 0);
    lv_obj_set_pos(lbl_spotify_state, text_x, 10);

    lbl_spotify_track = lv_label_create(panel);
    lv_label_set_text(lbl_spotify_track, "Nothing playing");
    lv_obj_set_style_text_font(lbl_spotify_track, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_spotify_track, COL_TEXT, 0);
    lv_label_set_long_mode(lbl_spotify_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_spotify_track, text_w);
    lv_obj_set_pos(lbl_spotify_track, text_x, 44);

    lbl_spotify_artist = lv_label_create(panel);
    lv_label_set_text(lbl_spotify_artist, "—");
    lv_obj_set_style_text_font(lbl_spotify_artist, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_spotify_artist, COL_DIM, 0);
    lv_label_set_long_mode(lbl_spotify_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_spotify_artist, text_w);
    lv_obj_set_pos(lbl_spotify_artist, text_x, 110);

    // Pixel-art dancer canvas — under the panel, centred.
    spotify_dance_buf = (uint16_t*)heap_caps_malloc(SP_DANCE_W * SP_DANCE_H * 2,
                                                    MALLOC_CAP_SPIRAM);
    if (spotify_dance_buf) {
        memset(spotify_dance_buf, 0, SP_DANCE_W * SP_DANCE_H * 2);
        spotify_dance_canvas = lv_canvas_create(spotify_container);
        lv_canvas_set_buffer(spotify_dance_canvas, spotify_dance_buf,
                             SP_DANCE_W, SP_DANCE_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(spotify_dance_canvas,
                       (SCR_W - SP_DANCE_W) / 2,
                       CONTENT_Y + 260 + 10);
        // Pick a Dance-category animation. Fall back to the first animation
        // if none is found (catalog could change after a re-scrape).
        spotify_dance_idx = -1;
        for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
            if (strcmp(splash_anims[i].category, "Dance") == 0) {
                spotify_dance_idx = (int8_t)i;
                break;
            }
        }
        if (spotify_dance_idx < 0 && SPLASH_ANIM_COUNT > 0) {
            spotify_dance_idx = 0;
        }
    }

    lv_obj_add_flag(spotify_container, LV_OBJ_FLAG_HIDDEN);
}

static void render_spotify_dance_frame(const splash_anim_def_t* a, int frame) {
    const uint8_t* cells = a->frames[frame];
    const uint16_t* palette = a->palette;
    for (int gy = 0; gy < SP_DANCE_GRID; gy++) {
        uint16_t row[SP_DANCE_W];
        for (int gx = 0; gx < SP_DANCE_GRID; gx++) {
            uint8_t code = cells[gy * SP_DANCE_GRID + gx];
            uint16_t color = (code < SPLASH_PALETTE_SIZE) ? palette[code] : 0x0000;
            uint16_t* p = &row[gx * SP_DANCE_CELL];
            for (int i = 0; i < SP_DANCE_CELL; i++) p[i] = color;
        }
        for (int dy = 0; dy < SP_DANCE_CELL; dy++) {
            memcpy(&spotify_dance_buf[(gy * SP_DANCE_CELL + dy) * SP_DANCE_W],
                   row, SP_DANCE_W * 2);
        }
    }
    if (spotify_dance_canvas) lv_obj_invalidate(spotify_dance_canvas);
}

// ======== Calendar Screen (480x480) ========

static void init_calendar_screen(lv_obj_t* scr) {
    calendar_container = lv_obj_create(scr);
    lv_obj_set_size(calendar_container, SCR_W, SCR_H);
    lv_obj_set_pos(calendar_container, 0, 0);
    lv_obj_set_style_bg_opa(calendar_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(calendar_container, 0, 0);
    lv_obj_set_style_pad_all(calendar_container, 0, 0);
    lv_obj_clear_flag(calendar_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(calendar_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(calendar_container);
    lv_label_set_text(title, "Up Next");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    lv_obj_t* panel = make_panel(calendar_container, MARGIN, CONTENT_Y, CONTENT_W, 260);

    // "UP NEXT" pill at the top of the panel; mirrors the Spotify status pill.
    lbl_calendar_state = lv_label_create(panel);
    lv_label_set_text(lbl_calendar_state, "CALENDAR");
    lv_obj_set_style_text_font(lbl_calendar_state, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_calendar_state, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl_calendar_state, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(lbl_calendar_state, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_calendar_state, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl_calendar_state, 12, 0);
    lv_obj_set_style_pad_right(lbl_calendar_state, 12, 0);
    lv_obj_set_style_pad_top(lbl_calendar_state, 4, 0);
    lv_obj_set_style_pad_bottom(lbl_calendar_state, 4, 0);
    lv_obj_set_pos(lbl_calendar_state, 0, 4);

    // Subtle accent-coloured vertical bar to the left of the event title.
    lv_obj_t* accent_bar = lv_obj_create(panel);
    lv_obj_set_size(accent_bar, 4, 100);
    lv_obj_set_pos(accent_bar, 0, 56);
    lv_obj_set_style_bg_color(accent_bar, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(accent_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(accent_bar, 0, 0);
    lv_obj_set_style_radius(accent_bar, 2, 0);
    lv_obj_clear_flag(accent_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_calendar_event = lv_label_create(panel);
    lv_label_set_text(lbl_calendar_event, "No upcoming events");
    lv_obj_set_style_text_font(lbl_calendar_event, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_calendar_event, COL_TEXT, 0);
    lv_label_set_long_mode(lbl_calendar_event, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_calendar_event, CONTENT_W - 32 - 18);
    lv_obj_set_pos(lbl_calendar_event, 18, 56);

    // Time-until pill at the bottom.
    lbl_calendar_when = lv_label_create(panel);
    lv_label_set_text(lbl_calendar_when, "\xE2\x80\x94");
    lv_obj_set_style_text_font(lbl_calendar_when, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_calendar_when, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl_calendar_when, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl_calendar_when, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_calendar_when, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl_calendar_when, 16, 0);
    lv_obj_set_style_pad_right(lbl_calendar_when, 16, 0);
    lv_obj_set_style_pad_top(lbl_calendar_when, 6, 0);
    lv_obj_set_style_pad_bottom(lbl_calendar_when, 6, 0);
    lv_obj_set_pos(lbl_calendar_when, 0, 190);

    lv_obj_add_flag(calendar_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Settings Screen (480x480) ========

static void on_auto_rotate_changed(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    Serial.printf("ui: auto_rotate switch -> %d\n", on ? 1 : 0);
    settings_set_auto_rotate(on);
}

// Whole-row tap target. The 70-px switch widget itself is too small to hit
// reliably on the 480-px touch panel, so tapping anywhere in the row flips
// the switch state and persists.
static void on_auto_rotate_row_clicked(lv_event_t* e) {
    (void)e;
    if (!sw_auto_rotate) return;
    bool now_on = !lv_obj_has_state(sw_auto_rotate, LV_STATE_CHECKED);
    if (now_on) lv_obj_add_state(sw_auto_rotate, LV_STATE_CHECKED);
    else        lv_obj_clear_state(sw_auto_rotate, LV_STATE_CHECKED);
    Serial.printf("ui: auto_rotate row tap -> %d\n", now_on ? 1 : 0);
    settings_set_auto_rotate(now_on);
}

static void on_bluetooth_row_clicked(lv_event_t* e) {
    (void)e;
    ui_show_screen(SCREEN_BLUETOOTH);
}

static lv_obj_t* make_settings_row(lv_obj_t* parent, int y, int h) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_pos(row, MARGIN, y);
    lv_obj_set_size(row, CONTENT_W, h);
    lv_obj_set_style_bg_color(row, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 18, 0);
    lv_obj_set_style_pad_right(row, 18, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void init_settings_screen(lv_obj_t* scr) {
    settings_container = lv_obj_create(scr);
    lv_obj_set_size(settings_container, SCR_W, SCR_H);
    lv_obj_set_pos(settings_container, 0, 0);
    lv_obj_set_style_bg_opa(settings_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_container, 0, 0);
    lv_obj_set_style_pad_all(settings_container, 0, 0);
    lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(settings_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(settings_container);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Row 1: auto-rotate (whole row is a tap target)
    int y = CONTENT_Y;
    int row_h = 80;
    lv_obj_t* row1 = make_settings_row(settings_container, y, row_h);
    lv_obj_add_event_cb(row1, on_auto_rotate_row_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l1 = lv_label_create(row1);
    lv_label_set_text(l1, "Auto-rotate screens");
    lv_obj_set_style_text_font(l1, &font_styrene_28, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_LEFT_MID, 0, 0);

    sw_auto_rotate = lv_switch_create(row1);
    lv_obj_set_size(sw_auto_rotate, 70, 36);
    lv_obj_align(sw_auto_rotate, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw_auto_rotate, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_auto_rotate, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_auto_rotate, on_auto_rotate_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Row 2: rotate interval (read-only display for now)
    y += row_h + 12;
    lv_obj_t* row2 = make_settings_row(settings_container, y, row_h);
    lv_obj_t* l2 = lv_label_create(row2);
    lv_label_set_text(l2, "Rotate every");
    lv_obj_set_style_text_font(l2, &font_styrene_28, 0);
    lv_obj_set_style_text_color(l2, COL_TEXT, 0);
    lv_obj_align(l2, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* l2v = lv_label_create(row2);
    char buf[16];
    snprintf(buf, sizeof(buf), "%us", (unsigned)settings_rotate_interval_sec());
    lv_label_set_text(l2v, buf);
    lv_obj_set_style_text_font(l2v, &font_styrene_28, 0);
    lv_obj_set_style_text_color(l2v, COL_DIM, 0);
    lv_obj_align(l2v, LV_ALIGN_RIGHT_MID, 0, 0);

    // Row 3: Bluetooth entry (tap → BT screen)
    y += row_h + 12;
    lv_obj_t* row3 = make_settings_row(settings_container, y, row_h);
    lv_obj_add_event_cb(row3, on_bluetooth_row_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l3 = lv_label_create(row3);
    lv_label_set_text(l3, "Bluetooth");
    lv_obj_set_style_text_font(l3, &font_styrene_28, 0);
    lv_obj_set_style_text_color(l3, COL_TEXT, 0);
    lv_obj_align(l3, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* l3a = lv_label_create(row3);
    lv_label_set_text(l3a, ">");
    lv_obj_set_style_text_font(l3a, &font_styrene_28, 0);
    lv_obj_set_style_text_color(l3a, COL_DIM, 0);
    lv_obj_align(l3a, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Pomodoro Screen (480x480) ========

static void pomodoro_click_cb(lv_event_t* e) {
    (void)e;
    pomodoro_toggle_pause();
}

static void pomodoro_long_press_cb(lv_event_t* e) {
    (void)e;
    pomodoro_reset();
}

static void init_pomodoro_screen(lv_obj_t* scr) {
    pomodoro_container = lv_obj_create(scr);
    lv_obj_set_size(pomodoro_container, SCR_W, SCR_H);
    lv_obj_set_pos(pomodoro_container, 0, 0);
    lv_obj_set_style_bg_opa(pomodoro_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pomodoro_container, 0, 0);
    lv_obj_set_style_pad_all(pomodoro_container, 0, 0);
    lv_obj_clear_flag(pomodoro_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pomodoro_container, pomodoro_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(pomodoro_container, pomodoro_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t* title = lv_label_create(pomodoro_container);
    lv_label_set_text(title, "Pomodoro");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Big circular progress arc with the countdown centred inside.
    pomodoro_arc = lv_arc_create(pomodoro_container);
    lv_obj_set_size(pomodoro_arc, 300, 300);
    lv_obj_align(pomodoro_arc, LV_ALIGN_TOP_MID, 0, CONTENT_Y - 20);
    lv_arc_set_range(pomodoro_arc, 0, 1000);
    lv_arc_set_bg_angles(pomodoro_arc, 0, 360);
    lv_arc_set_rotation(pomodoro_arc, 270);
    lv_arc_set_value(pomodoro_arc, 0);
    lv_obj_remove_style(pomodoro_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(pomodoro_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(pomodoro_arc, COL_BAR_BG,   LV_PART_MAIN);
    lv_obj_set_style_arc_color(pomodoro_arc, COL_ACCENT,   LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(pomodoro_arc, 14,           LV_PART_MAIN);
    lv_obj_set_style_arc_width(pomodoro_arc, 14,           LV_PART_INDICATOR);

    // State pill ("FOCUS" / "BREAK" / "READY").
    lbl_pomodoro_state = lv_label_create(pomodoro_container);
    lv_label_set_text(lbl_pomodoro_state, "READY");
    lv_obj_set_style_text_font(lbl_pomodoro_state, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_pomodoro_state, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl_pomodoro_state, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl_pomodoro_state, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_pomodoro_state, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl_pomodoro_state, 14, 0);
    lv_obj_set_style_pad_right(lbl_pomodoro_state, 14, 0);
    lv_obj_set_style_pad_top(lbl_pomodoro_state, 4, 0);
    lv_obj_set_style_pad_bottom(lbl_pomodoro_state, 4, 0);
    lv_obj_align(lbl_pomodoro_state, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 60);

    // Big mm:ss countdown.
    lbl_pomodoro_time = lv_label_create(pomodoro_container);
    lv_label_set_text(lbl_pomodoro_time, "25:00");
    lv_obj_set_style_text_font(lbl_pomodoro_time, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_pomodoro_time, COL_TEXT, 0);
    lv_obj_align(lbl_pomodoro_time, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 110);

    lbl_pomodoro_cycles = lv_label_create(pomodoro_container);
    lv_label_set_text(lbl_pomodoro_cycles, "Cycle 0");
    lv_obj_set_style_text_font(lbl_pomodoro_cycles, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_pomodoro_cycles, COL_DIM, 0);
    lv_obj_align(lbl_pomodoro_cycles, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 180);

    lbl_pomodoro_hint = lv_label_create(pomodoro_container);
    lv_label_set_text(lbl_pomodoro_hint, "Tap to start  /  hold to reset");
    lv_obj_set_style_text_font(lbl_pomodoro_hint, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_pomodoro_hint, COL_DIM, 0);
    lv_obj_align(lbl_pomodoro_hint, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_add_flag(pomodoro_container, LV_OBJ_FLAG_HIDDEN);
}

static void ui_refresh_pomodoro(void) {
    if (!pomodoro_arc) return;
    pomo_state_t st = pomodoro_state();
    uint32_t remaining = pomodoro_remaining_ms();
    uint32_t total = pomodoro_phase_total_ms();
    bool paused = pomodoro_paused();
    uint16_t cycles = pomodoro_completed_work();

    int mins = remaining / 60000;
    int secs = (remaining % 60000) / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    lv_label_set_text(lbl_pomodoro_time, buf);

    const char* state_text = "READY";
    lv_color_t state_bg = COL_BAR_BG;
    lv_color_t state_fg = COL_DIM;
    if (st == POMO_WORKING) {
        state_text = paused ? "FOCUS PAUSED" : "FOCUS";
        state_bg   = paused ? COL_BAR_BG : COL_ACCENT;
        state_fg   = paused ? COL_DIM    : COL_TEXT;
    } else if (st == POMO_BREAK) {
        state_text = paused ? "BREAK PAUSED" : "BREAK";
        state_bg   = paused ? COL_BAR_BG : COL_GREEN;
        state_fg   = COL_TEXT;
    }
    lv_label_set_text(lbl_pomodoro_state, state_text);
    lv_obj_set_style_bg_color(lbl_pomodoro_state, state_bg, 0);
    lv_obj_set_style_text_color(lbl_pomodoro_state, state_fg, 0);

    int arc_val = 0;
    if (total > 0 && st != POMO_IDLE) {
        // Progress shows time elapsed (so the arc fills as the phase advances).
        uint32_t elapsed = (total >= remaining) ? (total - remaining) : 0;
        arc_val = (int)((uint64_t)elapsed * 1000 / total);
    }
    lv_arc_set_value(pomodoro_arc, arc_val);
    lv_obj_set_style_arc_color(pomodoro_arc,
        st == POMO_BREAK ? COL_GREEN : COL_ACCENT, LV_PART_INDICATOR);

    char cbuf[24];
    if (cycles > 0) snprintf(cbuf, sizeof(cbuf), "Cycle %u", (unsigned)cycles);
    else            snprintf(cbuf, sizeof(cbuf), "-");
    lv_label_set_text(lbl_pomodoro_cycles, cbuf);

    if (st == POMO_IDLE) {
        lv_label_set_text(lbl_pomodoro_hint, "Tap to start  /  hold to reset");
    } else if (paused) {
        lv_label_set_text(lbl_pomodoro_hint, "Tap to resume  /  hold to reset");
    } else {
        lv_label_set_text(lbl_pomodoro_hint, "Tap to pause  /  hold to reset");
    }
}

// ======== Photo Screen (480x480) ========

static void init_photo_screen(lv_obj_t* scr) {
    photo_container = lv_obj_create(scr);
    lv_obj_set_size(photo_container, SCR_W, SCR_H);
    lv_obj_set_pos(photo_container, 0, 0);
    lv_obj_set_style_bg_color(photo_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(photo_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo_container, 0, 0);
    lv_obj_set_style_pad_all(photo_container, 0, 0);
    lv_obj_clear_flag(photo_container, LV_OBJ_FLAG_SCROLLABLE);

    // Full-screen RGB565 canvas backed by the BLE photo PSRAM buffer.
    photo_canvas = lv_canvas_create(photo_container);
    lv_canvas_set_buffer(photo_canvas, (void*)ble_photo_data(),
                         BLE_PHOTO_W, BLE_PHOTO_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(photo_canvas, 0, 0);
    lv_obj_set_style_pad_all(photo_canvas, 0, 0);
    lv_obj_set_style_border_width(photo_canvas, 0, 0);
    lv_obj_set_style_radius(photo_canvas, 0, 0);
    lv_obj_clear_flag(photo_canvas, LV_OBJ_FLAG_SCROLLABLE);
    // Show canvas immediately (with the zero-init black buffer) so the user
    // sees a steady black background instead of flicker, and `ui_set_photo_loading`
    // takes it over from there once a transfer is in flight.
    lv_obj_clear_flag(photo_canvas, LV_OBJ_FLAG_HIDDEN);

    // Caption pill bottom-left over a translucent panel so it stays legible
    // against any photo.
    photo_caption_box = lv_obj_create(photo_container);
    lv_obj_set_size(photo_caption_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_pos(photo_caption_box, 24, SCR_H - 60);
    lv_obj_set_style_bg_color(photo_caption_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(photo_caption_box, LV_OPA_70, 0);
    lv_obj_set_style_radius(photo_caption_box, 8, 0);
    lv_obj_set_style_border_width(photo_caption_box, 0, 0);
    lv_obj_set_style_pad_left(photo_caption_box, 14, 0);
    lv_obj_set_style_pad_right(photo_caption_box, 14, 0);
    lv_obj_set_style_pad_top(photo_caption_box, 6, 0);
    lv_obj_set_style_pad_bottom(photo_caption_box, 6, 0);
    lv_obj_clear_flag(photo_caption_box, LV_OBJ_FLAG_SCROLLABLE);

    lbl_photo_caption = lv_label_create(photo_caption_box);
    lv_label_set_text(lbl_photo_caption, "Waiting for photo...");
    lv_obj_set_style_text_font(lbl_photo_caption, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_photo_caption, lv_color_hex(0xffffff), 0);

    lv_obj_add_flag(photo_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Bluetooth Accessories Screen (480x480) ========

static void init_btacc_screen(lv_obj_t* scr) {
    btacc_container = lv_obj_create(scr);
    lv_obj_set_size(btacc_container, SCR_W, SCR_H);
    lv_obj_set_pos(btacc_container, 0, 0);
    lv_obj_set_style_bg_opa(btacc_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btacc_container, 0, 0);
    lv_obj_set_style_pad_all(btacc_container, 0, 0);
    lv_obj_clear_flag(btacc_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btacc_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(btacc_container);
    lv_label_set_text(title, "Accessories");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    btacc_list_parent = lv_obj_create(btacc_container);
    lv_obj_set_pos(btacc_list_parent, MARGIN, CONTENT_Y);
    lv_obj_set_size(btacc_list_parent, CONTENT_W, SCR_H - CONTENT_Y - MARGIN);
    lv_obj_set_style_bg_opa(btacc_list_parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btacc_list_parent, 0, 0);
    lv_obj_set_style_pad_all(btacc_list_parent, 0, 0);
    lv_obj_set_flex_flow(btacc_list_parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(btacc_list_parent, 10, 0);
    lv_obj_clear_flag(btacc_list_parent, LV_OBJ_FLAG_SCROLLABLE);

    btacc_empty_label = lv_label_create(btacc_container);
    lv_label_set_text(btacc_empty_label, "No connected accessories");
    lv_obj_set_style_text_font(btacc_empty_label, &font_styrene_28, 0);
    lv_obj_set_style_text_color(btacc_empty_label, COL_DIM, 0);
    lv_obj_align(btacc_empty_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_flag(btacc_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== AFK Overlay ========

static void render_afk_anim_frame(const splash_anim_def_t* a, int frame) {
    const uint8_t* cells = a->frames[frame];
    const uint16_t* palette = a->palette;
    for (int gy = 0; gy < AFK_ANIM_GRID; gy++) {
        uint16_t row[AFK_ANIM_W];
        for (int gx = 0; gx < AFK_ANIM_GRID; gx++) {
            uint8_t code = cells[gy * AFK_ANIM_GRID + gx];
            uint16_t color = (code < SPLASH_PALETTE_SIZE) ? palette[code] : 0x0000;
            uint16_t* p = &row[gx * AFK_ANIM_CELL];
            for (int i = 0; i < AFK_ANIM_CELL; i++) p[i] = color;
        }
        for (int dy = 0; dy < AFK_ANIM_CELL; dy++) {
            memcpy(&afk_anim_buf[(gy * AFK_ANIM_CELL + dy) * AFK_ANIM_W],
                   row, AFK_ANIM_W * 2);
        }
    }
    if (afk_canvas) lv_obj_invalidate(afk_canvas);
}

static void afk_click_cb(lv_event_t* e) {
    (void)e;
    ui_afk_dismiss();
}

static void init_afk_overlay(lv_obj_t* scr) {
    afk_layer = lv_obj_create(scr);
    lv_obj_set_size(afk_layer, SCR_W, SCR_H);
    lv_obj_set_pos(afk_layer, 0, 0);
    lv_obj_set_style_bg_color(afk_layer, COL_BG, 0);
    lv_obj_set_style_bg_opa(afk_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(afk_layer, 0, 0);
    lv_obj_set_style_pad_all(afk_layer, 0, 0);
    lv_obj_clear_flag(afk_layer, LV_OBJ_FLAG_SCROLLABLE);
    // Own click handler so a tap dismisses the overlay instead of falling
    // through to whatever screen is hidden underneath it.
    lv_obj_add_flag(afk_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(afk_layer, afk_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(afk_layer);
    lv_label_set_text(title, "Looks like I'm gone");
    lv_obj_set_style_text_font(title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, SCR_W - 40);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t* sub = lv_label_create(afk_layer);
    lv_label_set_text(sub, "Be back soon...");
    lv_obj_set_style_text_font(sub, &font_styrene_24, 0);
    lv_obj_set_style_text_color(sub, COL_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 200);

    afk_anim_buf = (uint16_t*)heap_caps_malloc(AFK_ANIM_W * AFK_ANIM_H * 2,
                                               MALLOC_CAP_SPIRAM);
    if (afk_anim_buf) {
        memset(afk_anim_buf, 0, AFK_ANIM_W * AFK_ANIM_H * 2);
        afk_canvas = lv_canvas_create(afk_layer);
        lv_canvas_set_buffer(afk_canvas, afk_anim_buf,
                             AFK_ANIM_W, AFK_ANIM_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(afk_canvas, LV_ALIGN_BOTTOM_MID, 0, -50);
        // Pick the "expression sleep" animation if available — most matches
        // the "I'm gone" mood. Fall back to any expression animation.
        for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
            if (strstr(splash_anims[i].name, "sleep")) {
                afk_anim_idx = (int8_t)i;
                break;
            }
        }
        if (afk_anim_idx < 0) {
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].category, "Expression") == 0) {
                    afk_anim_idx = (int8_t)i;
                    break;
                }
            }
        }
        if (afk_anim_idx < 0 && SPLASH_ANIM_COUNT > 0) afk_anim_idx = 0;
        if (afk_anim_idx >= 0) {
            render_afk_anim_frame(&splash_anims[afk_anim_idx], 0);
        }
    }

    lv_obj_add_flag(afk_layer, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Logo (shared, always visible, on top of all containers)
    // Logo is RGB565A8 (planar: w*h RGB565 then w*h alpha) so it composites
    // cleanly against whatever bg is behind it.
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    // Initialize battery icon descriptors
    init_battery_icons();

    init_usage_screen(scr);
    init_history_screen(scr);
    init_vacation_screen(scr);
    init_spotify_screen(scr);
    init_calendar_screen(scr);
    init_pomodoro_screen(scr);
    init_photo_screen(scr);
    init_btacc_screen(scr);
    init_settings_screen(scr);
    init_bluetooth_screen(scr);
    splash_init(scr);
    init_afk_overlay(scr);

    ui_refresh_settings();
    ui_refresh_pomodoro();

    // Splash is touch-toggled — tap anywhere on the splash dismisses it
    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Logo on top of all containers (inset for rounded corners)
    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, MARGIN, TITLE_Y - 10);

    // Battery indicator on top of all containers (upper-right, inset)
    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, SCR_W - 48 - MARGIN, TITLE_Y);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);

    // Usage screen
    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_update_spotify(const SpotifyData* data) {
    if (!data || !data->valid) {
        lv_label_set_text(lbl_spotify_state, "IDLE");
        lv_obj_set_style_bg_color(lbl_spotify_state, COL_BAR_BG, 0);
        lv_obj_set_style_text_color(lbl_spotify_state, COL_DIM, 0);
        lv_label_set_text(lbl_spotify_track, "Nothing playing");
        lv_label_set_text(lbl_spotify_artist, "\xE2\x80\x94");
        return;
    }
    if (data->playing) {
        lv_label_set_text(lbl_spotify_state, "NOW PLAYING");
        lv_obj_set_style_bg_color(lbl_spotify_state, COL_ACCENT, 0);
        lv_obj_set_style_text_color(lbl_spotify_state, COL_TEXT, 0);
    } else {
        lv_label_set_text(lbl_spotify_state, "PAUSED");
        lv_obj_set_style_bg_color(lbl_spotify_state, COL_BAR_BG, 0);
        lv_obj_set_style_text_color(lbl_spotify_state, COL_DIM, 0);
    }
    lv_label_set_text(lbl_spotify_track, data->track[0] ? data->track : "—");
    lv_label_set_text(lbl_spotify_artist, data->artist[0] ? data->artist : "—");

    // Mood-tint the panel from the album's dominant colour. We mix ~28 % of
    // the source with black so the panel stays dark enough for the foreground
    // text to keep contrast.
    if (data->has_color && spotify_panel) {
        lv_color_t tint = darken_rgb24(data->bg_color, 28);
        lv_obj_set_style_bg_color(spotify_panel, tint, 0);
    }
}

void ui_update_spotify_art(const uint8_t* rgb565, int w, int h) {
    if (!rgb565 || w <= 0 || h <= 0) return;
    spotify_art_dsc.header.w = w;
    spotify_art_dsc.header.h = h;
    spotify_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    spotify_art_dsc.header.stride = w * 2;
    spotify_art_dsc.data = rgb565;
    spotify_art_dsc.data_size = w * h * 2;
    // Re-set src every update so LVGL's image cache invalidates and re-reads
    // the (mutated-in-place) PSRAM buffer for the new track.
    lv_image_set_src(img_spotify_art, &spotify_art_dsc);
    spotify_art_seen = true;
    lv_obj_clear_flag(img_spotify_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(img_spotify_art);
}

static void format_when(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "—");
    } else if (mins == 0) {
        snprintf(buf, len, "Now");
    } else if (mins < 60) {
        snprintf(buf, len, "in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

void ui_refresh_settings(void) {
    if (!sw_auto_rotate) return;
    if (settings_auto_rotate()) lv_obj_add_state(sw_auto_rotate, LV_STATE_CHECKED);
    else                        lv_obj_clear_state(sw_auto_rotate, LV_STATE_CHECKED);
}

void ui_update_bt_accessories(const BTAccessoriesData* data) {
    if (!btacc_list_parent) return;
    lv_obj_clean(btacc_list_parent);
    if (!data || !data->valid || data->count == 0) {
        if (btacc_empty_label) lv_obj_clear_flag(btacc_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (btacc_empty_label) lv_obj_add_flag(btacc_empty_label, LV_OBJ_FLAG_HIDDEN);

    for (uint8_t i = 0; i < data->count && i < BT_ACC_MAX; i++) {
        const BTAccessory& a = data->items[i];

        lv_obj_t* row = lv_obj_create(btacc_list_parent);
        lv_obj_set_size(row, CONTENT_W, 56);
        lv_obj_set_style_bg_color(row, COL_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 18, 0);
        lv_obj_set_style_pad_right(row, 18, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, a.name);
        lv_obj_set_style_text_font(name_lbl, &font_styrene_24, 0);
        lv_obj_set_style_text_color(name_lbl, COL_TEXT, 0);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, CONTENT_W - 36 - 110);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        char pctbuf[16];
        if (a.battery_pct < 0) snprintf(pctbuf, sizeof(pctbuf), "—");
        else                   snprintf(pctbuf, sizeof(pctbuf), "%d%%%s",
                                        a.battery_pct, a.charging ? "+" : "");
        lv_obj_t* pct_lbl = lv_label_create(row);
        lv_label_set_text(pct_lbl, pctbuf);
        lv_obj_set_style_text_font(pct_lbl, &font_styrene_28, 0);
        // Tint by level: red ≤20, amber ≤50, green otherwise.
        lv_color_t c = COL_GREEN;
        if (a.battery_pct >= 0 && a.battery_pct <= 20)      c = COL_RED;
        else if (a.battery_pct >= 0 && a.battery_pct <= 50) c = COL_AMBER;
        else if (a.battery_pct < 0)                         c = COL_DIM;
        lv_obj_set_style_text_color(pct_lbl, c, 0);
        lv_obj_align(pct_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void ui_update_photo_caption(const PhotoData* data) {
    if (!lbl_photo_caption) return;
    if (!data || !data->valid || !data->caption[0]) {
        lv_label_set_text(lbl_photo_caption,
            photo_have_image ? " " : "Waiting for photo...");
        return;
    }
    lv_label_set_text(lbl_photo_caption, data->caption);
}

static bool logo_attention_on = false;
static int  logo_base_y = TITLE_Y - 10;

static void logo_bounce_exec(void* obj, int32_t v) {
    lv_obj_set_y((lv_obj_t*)obj, logo_base_y - v);
}

void ui_set_attention(bool on) {
    if (!logo_img) return;
    if (on == logo_attention_on) return;
    logo_attention_on = on;
    if (on) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, logo_img);
        lv_anim_set_values(&a, 0, 22);
        lv_anim_set_exec_cb(&a, logo_bounce_exec);
        lv_anim_set_time(&a, 260);
        lv_anim_set_playback_time(&a, 260);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        lv_anim_del(logo_img, logo_bounce_exec);
        lv_obj_set_y(logo_img, logo_base_y);
    }
}

void ui_set_afk(bool on) {
    if (!afk_layer) return;
    if (on == afk_is_on) return;
    afk_is_on = on;
    if (on) {
        lv_obj_clear_flag(afk_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(afk_layer);
    } else {
        lv_obj_add_flag(afk_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_afk_active(void) { return afk_is_on; }

void ui_afk_dismiss(void) {
    afk_dismissed_ms = millis();
    if (afk_dismissed_ms == 0) afk_dismissed_ms = 1;  // 0 means "never dismissed"
    ui_set_afk(false);
}

bool ui_afk_suppressed(void) {
    if (afk_dismissed_ms == 0) return false;
    return (millis() - afk_dismissed_ms) < AFK_SUPPRESS_MS;
}

void ui_update_photo_image(const uint8_t* rgb565) {
    if (!photo_canvas || !rgb565) return;
    // Canvas buffer already points at the BLE photo PSRAM region, so the
    // pixels are in place — we just invalidate to redraw.
    photo_have_image = true;
    lv_obj_clear_flag(photo_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(photo_canvas);
}

// Toggle the photo caption only — leave the canvas visible so the user sees
// the previous (intact) image while a new one is mid-transfer. Tearing is
// limited to small strip overlap and is mostly invisible at 60 fps; hiding
// the canvas turned out to leave the user staring at black for the whole
// transfer window.
void ui_set_photo_loading(bool loading) {
    if (!lbl_photo_caption) return;
    if (loading) {
        lv_label_set_text(lbl_photo_caption, "Loading...");
    }
}

void ui_update_calendar(const CalendarData* data) {
    if (!data || !data->valid || !data->title[0]) {
        lv_label_set_text(lbl_calendar_state, "CALENDAR");
        lv_label_set_text(lbl_calendar_event, "No upcoming events");
        lv_label_set_text(lbl_calendar_when, "\xE2\x80\x94");
        return;
    }
    lv_label_set_text(lbl_calendar_state, "UP NEXT");
    lv_label_set_text(lbl_calendar_event, data->title);
    char buf[48];
    format_when(data->mins_until, buf, sizeof(buf));
    lv_label_set_text(lbl_calendar_when, buf);
}

void ui_tick_anim(void) {
    uint32_t now = lv_tick_get();

    if (current_screen == SCREEN_USAGE) {
        if (now - anim_msg_start >= ANIM_MSG_MS) {
            anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
            anim_msg_start = now;
        }

        if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
            anim_last_ms = now;
            anim_phase = (anim_phase + 1) % SPINNER_PHASES;
            anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                            : (SPINNER_PHASES - anim_phase);

            static char buf[80];
            snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                     spinner_frames[anim_spinner_idx],
                     anim_messages[anim_msg_idx]);
            lv_label_set_text(lbl_anim, buf);
        }
    } else if (current_screen == SCREEN_SPOTIFY) {
        if (spotify_dance_idx < 0 || !spotify_dance_canvas) return;
        const splash_anim_def_t* a = &splash_anims[spotify_dance_idx];
        if (a->frame_count == 0) return;
        uint16_t hold = a->holds[spotify_dance_frame];
        if (spotify_dance_last_ms == 0 || now - spotify_dance_last_ms >= hold) {
            spotify_dance_last_ms = now;
            spotify_dance_frame = (spotify_dance_frame + 1) % a->frame_count;
            render_spotify_dance_frame(a, spotify_dance_frame);
        }
    } else if (current_screen == SCREEN_POMODORO) {
        // Cheap refresh — 4 Hz keeps the seconds digit snappy without thrash.
        static uint32_t last_pomo_refresh = 0;
        if (now - last_pomo_refresh >= 250) {
            last_pomo_refresh = now;
            ui_refresh_pomodoro();
        }
    }

    // AFK animation runs whenever the overlay is showing, on top of whatever
    // screen is underneath.
    if (afk_is_on && afk_anim_idx >= 0 && afk_canvas) {
        const splash_anim_def_t* a = &splash_anims[afk_anim_idx];
        if (a->frame_count > 0) {
            uint16_t hold = a->holds[afk_anim_frame];
            if (afk_anim_last_ms == 0 || now - afk_anim_last_ms >= hold) {
                afk_anim_last_ms = now;
                afk_anim_frame = (afk_anim_frame + 1) % a->frame_count;
                render_afk_anim_frame(a, afk_anim_frame);
            }
        }
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
// Hide the battery indicator on the splash screen — the icon is visually
// noisy over the pixel-art creature animations.
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// LVGL handles click debouncing internally. Screen-level handler fires when
// no child consumed the event (children only consume if they have their own
// event callback, e.g. the Reset Bluetooth zone). On BT screen we skip the
// splash toggle so only the reset zone is interactive there.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    screen_t s = ui_get_current_screen();
    // Splash-as-screensaver: only the Usage screen taps in, and only the
    // active splash taps out. Spotify / Calendar / Settings / Bluetooth all
    // have interactive widgets and shouldn't be hijacked by a stray tap.
    if (s != SCREEN_USAGE && s != SCREEN_SPLASH) return;
    ui_toggle_splash();
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (history_container) lv_obj_add_flag(history_container, LV_OBJ_FLAG_HIDDEN);
    if (vacation_container) lv_obj_add_flag(vacation_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spotify_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(calendar_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pomodoro_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(photo_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btacc_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_HISTORY:    if (history_container) { lv_obj_clear_flag(history_container, LV_OBJ_FLAG_HIDDEN); render_history_graph(); } break;
    case SCREEN_VACATION:   if (vacation_container) lv_obj_clear_flag(vacation_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SPOTIFY:    lv_obj_clear_flag(spotify_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_CALENDAR:   lv_obj_clear_flag(calendar_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_POMODORO:   lv_obj_clear_flag(pomodoro_container, LV_OBJ_FLAG_HIDDEN); ui_refresh_pomodoro(); break;
    case SCREEN_PHOTO:      lv_obj_clear_flag(photo_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BTACC:      lv_obj_clear_flag(btacc_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SETTINGS:   lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Hide the logo overlay on the splash screen so the animation has a clean canvas
    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    // Bluetooth is intentionally outside the cycle — reachable only from
    // the Settings screen entry. Cycling stays on screens with real-time
    // content.
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_HISTORY;   break;
    case SCREEN_HISTORY:   next = SCREEN_VACATION;  break;
    case SCREEN_VACATION:  next = SCREEN_SPOTIFY;   break;
    case SCREEN_SPOTIFY:   next = SCREEN_CALENDAR;  break;
    case SCREEN_CALENDAR:  next = SCREEN_POMODORO;  break;
    case SCREEN_POMODORO:  next = SCREEN_PHOTO;     break;
    case SCREEN_PHOTO:     next = SCREEN_BTACC;     break;
    case SCREEN_BTACC:     next = SCREEN_SETTINGS;  break;
    case SCREEN_SETTINGS:  next = SCREEN_USAGE;     break;
    case SCREEN_BLUETOOTH: next = SCREEN_SETTINGS;  break;
    default:               next = SCREEN_USAGE;     break;
    }
    ui_show_screen(next);
}

void ui_cycle_screen_auto(void) {
    // Auto-rotate stays on content screens only — Settings is config-only.
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_HISTORY; break;
    case SCREEN_HISTORY:   next = SCREEN_VACATION; break;
    case SCREEN_VACATION:  next = SCREEN_SPOTIFY; break;
    case SCREEN_SPOTIFY:   next = SCREEN_CALENDAR; break;
    case SCREEN_CALENDAR:  next = SCREEN_POMODORO; break;
    case SCREEN_POMODORO:  next = SCREEN_PHOTO;    break;
    case SCREEN_PHOTO:     next = SCREEN_BTACC;    break;
    case SCREEN_BTACC:     next = SCREEN_USAGE;    break;
    default:               next = SCREEN_USAGE;    break;
    }
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    // Always pick the icon by level so the fill stays visible while charging.
    // Previously charging forced a bolt icon (idx 4) that hid how full the
    // battery was; now we keep the level shape and tint it green to signal
    // charging — shape shows charge %, colour shows charging state.
    int idx;
    if (percent < 0) {
        idx = 0;  // no battery / unknown
    } else if (percent <= 10) {
        idx = 0;  // empty
    } else if (percent <= 35) {
        idx = 1;  // low
    } else if (percent <= 75) {
        idx = 2;  // medium
    } else {
        idx = 3;  // full
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    if (charging && percent >= 0) {
        lv_obj_set_style_image_recolor(battery_img, COL_GREEN, 0);
        lv_obj_set_style_image_recolor_opa(battery_img, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_image_recolor_opa(battery_img, LV_OPA_TRANSP, 0);
    }
    apply_battery_visibility();
}
