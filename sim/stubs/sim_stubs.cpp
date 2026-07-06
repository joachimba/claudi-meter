// Stubs for firmware hardware modules (ble / power / imu) so the sim links.
// Behaviour mirrors a connected, charged, upright device.
#include <Arduino.h>
#include "ble.h"
#include "power.h"
#include "imu.h"

SerialSink Serial;

// ---- BLE stubs ---------------------------------------------------------
static const char* SIM_MAC = "AA:BB:CC:DD:EE:FF";
static const char* SIM_NAME = "Claude Controller (sim)";
static uint8_t sim_img_buf[BLE_ART_W * BLE_ART_H * 2] = {0};

void ble_init(void) {}
void ble_tick(void) {}
ble_state_t ble_get_state(void) { return BLE_STATE_CONNECTED; }
const char* ble_get_device_name(void) { return SIM_NAME; }
const char* ble_get_mac_address(void) { return SIM_MAC; }
void ble_clear_bonds(void) {}
bool ble_has_data(void) { return false; }
const char* ble_get_data(void) { return ""; }
void ble_send_ack(void) {}
void ble_send_nack(void) {}
void ble_request_refresh(void) {}
bool ble_image_ready(void) { return false; }
const uint8_t* ble_image_data(void) { return sim_img_buf; }
void ble_image_consume(void) {}

static uint8_t sim_photo_buf[BLE_PHOTO_W * BLE_PHOTO_H * 2] = {0};
bool ble_photo_ready(void) { return false; }
const uint8_t* ble_photo_data(void) { return sim_photo_buf; }
void ble_photo_consume(void) {}
void ble_keyboard_press(uint8_t, uint8_t) {}
void ble_keyboard_release(void) {}

void ble_send_cmd(uint8_t code) {
    fprintf(stdout, "[sim] ble_send_cmd: 0x%02x\n", code);
    fflush(stdout);
}

// ---- Power stubs -------------------------------------------------------
void power_init(void) {}
void power_tick(void) {}
int  power_battery_pct(void)  { return 82; }
bool power_is_charging(void)  { return false; }
bool power_pwr_pressed(void)  { return false; }

// ---- IMU stubs ---------------------------------------------------------
void    imu_init(void) {}
void    imu_tick(void) {}
uint8_t imu_get_rotation(void) { return 0; }
