#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <esp_heap_caps.h>

#define DEVICE_NAME "Claude Controller"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request
#define IMG_CHAR_UUID       "4c41555a-4465-7669-6365-000000000005"  // album art chunks
#define CMD_CHAR_UUID       "4c41555a-4465-7669-6365-000000000006"  // device-initiated commands (1-byte notify)
#define VACIMG_CHAR_UUID    "4c41555a-4465-7669-6365-000000000008"  // vacation backdrop chunks

#define BLE_BUF_SIZE 512
#define BLE_IMG_BYTES    (BLE_ART_W    * BLE_ART_H    * 2)
#define BLE_VACIMG_BYTES (BLE_VACIMG_W * BLE_VACIMG_H * 2)

// IMG frame header: 6 bytes LE
//   [0..1] track_hash    — daemon-chosen ID per track URL
//   [2..3] byte_offset   — destination offset in the image buffer
//   [4..5] total_bytes   — full image byte count (= BLE_IMG_BYTES)
// followed by variable-length RGB565 payload bytes (raster order, lo-byte first).
#define IMG_HDR_SIZE 6

// VACIMG frame header: 10 bytes LE — wider offset/total so the 460 KB
// full-screen frame fits.
//   [0..1] image_hash
//   [2..5] byte_offset   (uint32_t)
//   [6..9] total_bytes   (uint32_t, = BLE_VACIMG_BYTES)
#define VACIMG_HDR_SIZE 10

// HID keyboard report descriptor (standard 6-KRO boot-protocol-compatible).
// Includes the LED output report (Num/Caps/Scroll Lock indicators) — without
// it macOS's Keyboard Setup Assistant flags the device as "unidentifiable"
// because the descriptor doesn't look like a complete keyboard.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224) - Left Control
    0x29, 0xE7,  //   Usage Maximum (231) - Right GUI
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant) - Reserved byte
    // LED output report — required for macOS to treat this as a full keyboard.
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data, Variable, Absolute) - LED report
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output (Constant) - LED report padding
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x65,  //   Logical Maximum (101)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x65,  //   Usage Maximum (101)
    0x81, 0x00,  //   Input (Data, Array) - Key array (6 keys)
    0xC0,        // End Collection
};

static NimBLEServer* server = nullptr;
static NimBLEHIDDevice* hid_dev = nullptr;
static NimBLECharacteristic* input_kbd = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;
static NimBLECharacteristic* img_char = nullptr;
static NimBLECharacteristic* cmd_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;
static char rx_buf[BLE_BUF_SIZE];
static volatile bool data_ready = false;
static volatile bool has_received_data = false;
static char mac_str[18];

// Image reassembly state.
// Daemon writes IMG frames as [hash:2 LE][offset:2 LE][total:2 LE][payload...].
// Hash is a 16-bit ID for the current track URL (anti-collision: we drop the
// in-flight buffer when the daemon switches to a new hash). The receive
// counter advances by payload length per chunk; once it reaches total we
// flip ready. Writes are sent response=true so order + delivery are
// guaranteed by the host stack.
static uint8_t*  img_buf = nullptr;
static uint16_t  img_active_hash = 0;
static uint16_t  img_expected_total = 0;
static uint32_t  img_received_bytes = 0;
static volatile bool img_ready = false;

// Vacation backdrop — same reassembly machinery as IMG at full-screen scale.
static NimBLECharacteristic* vacimg_char = nullptr;
static uint8_t*  vacimg_buf = nullptr;
static uint16_t  vacimg_active_hash = 0;
static uint32_t  vacimg_expected_total = 0;
static uint32_t  vacimg_received_bytes = 0;
static volatile bool vacimg_ready = false;

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet (≤31 bytes):
    //   flags (3) + appearance (4) + HID service 0x1812 (4) + name "Claude Controller" (19)
    //   = 30 bytes. macOS Bluetooth Settings only surfaces BLE-only devices
    //   that explicitly advertise the standard HID service UUID (0x1812) —
    //   without it the device is recognized internally but hidden from the
    //   GUI nearby-devices list.
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));  // BLE HID Service
    adv->setName(DEVICE_NAME);
    // Scan response carries the 128-bit custom data-service UUID for active
    // scanners (the host daemon scans actively).
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);

    bool ok = adv->start();
    // Only reflect ADVERTISING in the UI state when no client is connected.
    // With MAX_CONNECTIONS=2, onConnect re-advertises to fill the second slot;
    // without this guard the UI would flip CONNECTED → ADVERTISING on every
    // first connect and never come back until a second client arrived.
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        Serial.printf("BLE: connected from %s (active=%u)\n",
            info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());
        // Keep advertising while a connection slot is still free so a second
        // central (e.g. the host daemon alongside an OS-held HID link) can
        // discover and connect. NimBLE auto-stops advertising on each accept.
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        Serial.printf("BLE: disconnected (reason=%d, remaining=%u)\n",
            reason, (unsigned)s->getConnectedCount());
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        std::string val = chr->getValue();
        size_t len = val.length();
        if (len >= BLE_BUF_SIZE) len = BLE_BUF_SIZE - 1;
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        has_received_data = true;
    }
};

// When the daemon enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the daemon's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

class VacimgCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        if (!vacimg_buf) return;
        std::string val = chr->getValue();
        const uint8_t* p = (const uint8_t*)val.data();
        size_t len = val.length();
        if (len < VACIMG_HDR_SIZE) return;

        uint16_t hash =
            (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        uint32_t offset =
            (uint32_t)p[2] | ((uint32_t)p[3] << 8) |
            ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
        uint32_t total =
            (uint32_t)p[6] | ((uint32_t)p[7] << 8) |
            ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 24);
        size_t payload_len = len - VACIMG_HDR_SIZE;

        if (total != BLE_VACIMG_BYTES) return;
        if (offset + payload_len > BLE_VACIMG_BYTES) return;

        if (hash != vacimg_active_hash || offset == 0) {
            vacimg_active_hash = hash;
            vacimg_expected_total = total;
            vacimg_received_bytes = 0;
            vacimg_ready = false;
        }
        memcpy(vacimg_buf + offset, p + VACIMG_HDR_SIZE, payload_len);
        vacimg_received_bytes += payload_len;
        if (vacimg_received_bytes >= vacimg_expected_total) {
            vacimg_ready = true;
        }
    }
};

class ImgCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        if (!img_buf) return;  // PSRAM alloc failed at init
        std::string val = chr->getValue();
        const uint8_t* p = (const uint8_t*)val.data();
        size_t len = val.length();
        if (len < IMG_HDR_SIZE) return;

        uint16_t hash   = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        uint16_t offset = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
        uint16_t total  = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
        size_t payload_len = len - IMG_HDR_SIZE;

        if (total != BLE_IMG_BYTES) {
            // Daemon and firmware disagree on art dimensions — ignore.
            return;
        }
        if ((uint32_t)offset + payload_len > BLE_IMG_BYTES) return;

        if (hash != img_active_hash || offset == 0) {
            img_active_hash = hash;
            img_expected_total = total;
            img_received_bytes = 0;
            img_ready = false;
        }

        memcpy(img_buf + offset, p + IMG_HDR_SIZE, payload_len);
        img_received_bytes += payload_len;

        if (img_received_bytes >= img_expected_total) {
            img_ready = true;
        }
    }
};

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC
    // Bump preferred MTU so a single write fits the extended payload
    // (usage + spotify + calendar can run to ~250 bytes of JSON).
    NimBLEDevice::setMTU(247);

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- HID keyboard service ---
    hid_dev = new NimBLEHIDDevice(server);
    hid_dev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid_dev->setManufacturer("Anthropic");
    // PnP ID: (vendorIdSource, vendorId, productId, version).
    // Source 1 = Bluetooth SIG, vendor 0x02E5 = Espressif. Originally claimed
    // Apple's USB vendor 0x05AC + Magic Keyboard product 0x820A — macOS
    // validates Apple-claimed HIDs against known device IDs and silently
    // refuses to surface a Connect button for spoofers.
    hid_dev->setPnp(0x01, 0x02E5, 0x0001, 0x0100);
    // country=33 (US ANSI). Setting this to 0 ("not supported") causes macOS
    // to launch the Keyboard Setup Assistant on first pair asking the user
    // to identify the layout — we only ever send Space / Shift+Tab so the
    // physical layout is irrelevant; advertise a known one to skip the wizard.
    hid_dev->setHidInfo(33, 0x02);
    hid_dev->setBatteryLevel(100);
    input_kbd = hid_dev->getInputReport(1);  // report ID 1

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    img_char = svc->createCharacteristic(
        IMG_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    static ImgCallbacks imgCb;
    img_char->setCallbacks(&imgCb);

    cmd_char = svc->createCharacteristic(
        CMD_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    img_buf = (uint8_t*)heap_caps_malloc(BLE_IMG_BYTES, MALLOC_CAP_SPIRAM);
    if (!img_buf) {
        Serial.println("BLE: img_buf PSRAM alloc failed");
    } else {
        memset(img_buf, 0, BLE_IMG_BYTES);
    }

    vacimg_char = svc->createCharacteristic(
        VACIMG_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    static VacimgCallbacks vacimgCb;
    vacimg_char->setCallbacks(&vacimgCb);
    vacimg_buf = (uint8_t*)heap_caps_malloc(BLE_VACIMG_BYTES, MALLOC_CAP_SPIRAM);
    if (!vacimg_buf) {
        Serial.println("BLE: vacimg_buf PSRAM alloc failed");
    } else {
        memset(vacimg_buf, 0, BLE_VACIMG_BYTES);
    }

    svc->start();
    server->start();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    NimBLEDevice::deleteAllBonds();
    Serial.println("BLE: bonds cleared");
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    data_ready = false;
    return rx_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

bool ble_image_ready(void) {
    return img_ready;
}

const uint8_t* ble_image_data(void) {
    return img_buf;
}

void ble_image_consume(void) {
    img_ready = false;
}

bool ble_vacimg_ready(void)           { return vacimg_ready; }
const uint8_t* ble_vacimg_data(void)  { return vacimg_buf; }
void ble_vacimg_consume(void)         { vacimg_ready = false; }

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_send_cmd(uint8_t code) {
    if (state != BLE_STATE_CONNECTED || !cmd_char) return;
    cmd_char->setValue(&code, 1);
    cmd_char->notify();
    Serial.printf("BLE: cmd 0x%02x\n", code);
}

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    // HID report: [modifier, reserved, key1, key2, key3, key4, key5, key6]
    uint8_t report[8] = {modifier, 0, key, 0, 0, 0, 0, 0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}

void ble_keyboard_release(void) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    uint8_t report[8] = {0};
    input_kbd->setValue(report, sizeof(report));
    input_kbd->notify();
}
