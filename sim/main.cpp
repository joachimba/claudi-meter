// Desktop simulator entry point.
//
// Renders the firmware's LVGL UI into an SDL2 window. Keyboard shortcuts
// cycle between screens; mouse acts as touch. Fake usage / Spotify /
// calendar data is injected at startup so every screen has content.
//
// Controls:
//   LEFT/RIGHT previous / next page (same as the device's side buttons)
//   0 .. 7     jump to a screen by screen_t index (0 = splash)
//   SPACE      next page
//   a          load gradient placeholder into Spotify album art
//   p          toggle Spotify play/paused state
//   r          re-randomize usage % / calendar minutes
//   q / ESC    quit
//   mouse      acts as touch input

#include <SDL2/SDL.h>
#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "data.h"
#include "ui.h"

static constexpr int W = 480;
static constexpr int H = 480;

// Forward decl that matches firmware ble.h
#define BLE_ART_W 120
#define BLE_ART_H 120

static SDL_Window*   g_win = nullptr;
static SDL_Renderer* g_ren = nullptr;
static SDL_Texture*  g_tex = nullptr;
static uint16_t      g_fb[W * H];

static int  g_mouse_x = 0;
static int  g_mouse_y = 0;
static bool g_mouse_down = false;

static UsageData    g_usage{};
static SpotifyData  g_spotify{};
static CalendarData g_calendar{};
static uint8_t      g_art_buf[BLE_ART_W * BLE_ART_H * 2];

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int x1 = area->x1, y1 = area->y1;
    int w  = area->x2 - x1 + 1;
    int h  = area->y2 - y1 + 1;
    auto* src = reinterpret_cast<uint16_t*>(px_map);
    for (int y = 0; y < h; y++) {
        std::memcpy(&g_fb[(y1 + y) * W + x1], &src[y * w], w * 2);
    }
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_cb(void) {
    return (uint32_t)SDL_GetTicks();
}

static void fill_gradient_art(uint8_t* buf, int w, int h, int seed) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255) / w);
            uint8_t g = (uint8_t)((y * 255) / h);
            uint8_t b = (uint8_t)((seed * 37) & 0xFF);
            uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            int i = (y * w + x) * 2;
            buf[i]     = v & 0xFF;
            buf[i + 1] = (v >> 8) & 0xFF;
        }
    }
}

static void make_fake_data(int variant) {
    g_usage.session_pct = (float)((variant * 13 + 47) % 100);
    g_usage.session_reset_mins = 142 + (variant * 17 % 60);
    g_usage.weekly_pct = (float)((variant * 7 + 23) % 100);
    g_usage.weekly_reset_mins = 5 * 1440 + ((variant * 31) % 1440);
    std::strncpy(g_usage.status, "allowed", sizeof(g_usage.status) - 1);
    g_usage.ok = true;
    g_usage.valid = true;

    std::strncpy(g_spotify.track,
                 "Bohemian Rhapsody — 2011 Remaster",
                 sizeof(g_spotify.track) - 1);
    std::strncpy(g_spotify.artist, "Queen", sizeof(g_spotify.artist) - 1);
    g_spotify.playing = (variant % 2 == 0);
    g_spotify.valid = true;
    // Fake a mood-tint colour. Cycle hues with `variant` for quick visual tests.
    {
        uint8_t hues[][3] = {
            {0xD9, 0x77, 0x57}, // terra-cotta
            {0x4F, 0x8A, 0xC6}, // blue
            {0x6E, 0xC0, 0x7A}, // green
            {0xC2, 0x4A, 0x7A}, // pink
            {0xE0, 0xB5, 0x3A}, // amber
        };
        auto& c = hues[variant % (sizeof(hues) / sizeof(hues[0]))];
        g_spotify.bg_color = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
        g_spotify.has_color = true;
    }

    std::strncpy(g_calendar.title, "Stand-up with team",
                 sizeof(g_calendar.title) - 1);
    g_calendar.mins_until = 12 + (variant * 5 % 50);
    g_calendar.valid = true;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    g_win = SDL_CreateWindow("Clawdmeter Simulator",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             W, H, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, W, H);

    lv_init();
    lv_tick_set_cb(tick_cb);

    static uint16_t buf1[W * 60];
    static uint16_t buf2[W * 60];

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    ui_init();

    int variant = 0;
    make_fake_data(variant);
    fill_gradient_art(g_art_buf, BLE_ART_W, BLE_ART_H, variant);
    ui_update(&g_usage);
    ui_update_spotify(&g_spotify);
    ui_update_calendar(&g_calendar);
    ui_update_spotify_art(g_art_buf, BLE_ART_W, BLE_ART_H);
    ui_show_screen(SCREEN_USAGE);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_MOUSEMOTION:
                g_mouse_x = ev.motion.x;
                g_mouse_y = ev.motion.y;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) g_mouse_down = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) g_mouse_down = false;
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_q: case SDLK_ESCAPE: running = false; break;
                case SDLK_0: case SDLK_1: case SDLK_2: case SDLK_3:
                case SDLK_4: case SDLK_5: case SDLK_6: case SDLK_7:
                    ui_show_screen((screen_t)(ev.key.keysym.sym - SDLK_0));
                    break;
                case SDLK_LEFT:  ui_step_screen(-1, false); break;
                case SDLK_RIGHT: case SDLK_SPACE: ui_step_screen(+1, false); break;
                case SDLK_a:
                    variant = (variant + 1) % 16;
                    fill_gradient_art(g_art_buf, BLE_ART_W, BLE_ART_H, variant);
                    ui_update_spotify_art(g_art_buf, BLE_ART_W, BLE_ART_H);
                    break;
                case SDLK_p:
                    g_spotify.playing = !g_spotify.playing;
                    ui_update_spotify(&g_spotify);
                    break;
                case SDLK_r:
                    variant++;
                    make_fake_data(variant);
                    ui_update(&g_usage);
                    ui_update_spotify(&g_spotify);
                    ui_update_calendar(&g_calendar);
                    break;
                default: break;
                }
                break;
            }
        }

        lv_timer_handler();
        ui_tick_anim();

        SDL_UpdateTexture(g_tex, nullptr, g_fb, W * 2);
        SDL_RenderClear(g_ren);
        SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
        SDL_RenderPresent(g_ren);

        SDL_Delay(5);
    }

    SDL_DestroyTexture(g_tex);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
