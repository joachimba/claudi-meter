#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

// Album art (RGB565, fixed dimensions ART_W x ART_H). Set by chunked writes
// to the IMG characteristic; ble_image_ready() flips true once a full frame
// for a given track hash arrives, and clears on ble_image_consume().
#define BLE_ART_W 120
#define BLE_ART_H 120
bool ble_image_ready(void);
const uint8_t* ble_image_data(void);   // RGB565, BLE_ART_W*BLE_ART_H*2 bytes
void ble_image_consume(void);          // clear the ready flag

// Full-screen ambient photo (RGB565). Reassembled from chunks on a separate
// PHOTO characteristic — same framing as IMG but with 32-bit offset/total so
// half-megabyte payloads fit. ble_photo_ready() flips true on a fully-
// received frame and clears on ble_photo_consume().
#define BLE_PHOTO_W 480
#define BLE_PHOTO_H 480
bool ble_photo_ready(void);
bool ble_photo_transferring(void);     // true while chunks are mid-flight
const uint8_t* ble_photo_data(void);
void ble_photo_consume(void);

// Vacation backdrop (RGB565, 480x480) — same chunk framing as PHOTO on its
// own characteristic + buffer, so the countdown screen keeps a stable image.
bool ble_vacimg_ready(void);
const uint8_t* ble_vacimg_data(void);  // RGB565, BLE_PHOTO_W*BLE_PHOTO_H*2 bytes
void ble_vacimg_consume(void);

// Device→daemon commands over CMD characteristic. Single-byte payload.
enum ble_cmd_t : uint8_t {
    BLE_CMD_SPOTIFY_PLAYPAUSE = 0x01,
    BLE_CMD_SPOTIFY_NEXT      = 0x02,
    BLE_CMD_SPOTIFY_PREV      = 0x03,
};
void ble_send_cmd(uint8_t code);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
