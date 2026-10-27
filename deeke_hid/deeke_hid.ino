/**
 * DeekeScript 蓝牙 Hid 固件（ESP32-C3）。
 *
 * 手机作为主机连接本机：HID 服务负责点击、滑动和按键，
 * 自定义服务接收指令。蓝牙名由 flash.sh 写入下面的 DKNM 槽，
 * 未指定时为 DeekeScript- 加 MAC 后 4 位。
 *
 * 点击用触摸屏报告，集合放在描述符最前，否则 vivo 只登记键盘。
 * 描述符里带接触编号，内核会标成直接触摸，系统就按触摸屏处理，不再画鼠标箭头。
 * 横轴按屏幕宽度、纵轴按屏幕高度各自映射到 0..32767。
 * 蓝牙外观仍是键盘，列表图标和别人的板子一致。
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include "hid_types.h"

#include <cmath>
#include <cstring>

static const uint16_t FW_VER = 7;
static const char* CMD_SERVICE_UUID = "444b4844-0000-4000-8000-4445454b4501";
static const char* CMD_CHAR_UUID = "444b4844-0000-4000-8000-4445454b4502";
static const char* DATA_CHAR_UUID = "444b4844-0000-4000-8000-4445454b4503";

static const uint8_t CMD_TAP = 0x01;
static const uint8_t CMD_DOWN = 0x02;
static const uint8_t CMD_MOVE = 0x03;
static const uint8_t CMD_UP = 0x04;
static const uint8_t CMD_UP_XY = 0x05;
static const uint8_t CMD_SWIPE = 0x06;
static const uint8_t CMD_SWIPEX = 0x07;
static const uint8_t CMD_KEY_DOWN = 0x10;
static const uint8_t CMD_KEY_UP = 0x11;
static const uint8_t CMD_KEY_PRESS = 0x12;
static const uint8_t CMD_KEY_UP_ALL = 0x13;
static const uint8_t CMD_COMBO = 0x14;
static const uint8_t CMD_CONSUMER = 0x15;
static const uint8_t CMD_MOD_DOWN = 0x16;
static const uint8_t CMD_MOD_UP = 0x17;
static const uint8_t CMD_CONSUMER_HOLD = 0x18;
static const uint8_t CMD_CONSUMER_DOWN = 0x1A;
static const uint8_t CMD_CONSUMER_UP = 0x1B;
static const uint8_t CMD_SET_XY = 0x20;
static const uint8_t CMD_SET_RND = 0x21;
static const uint8_t CMD_SET_BAT = 0x22;
static const uint8_t CMD_REG = 0x23;
static const uint8_t CMD_REBOOT = 0x30;
static const uint8_t CMD_VER = 0x31;
static const uint8_t CMD_GET = 0x32;
static const uint8_t CMD_RAW = 0x40;

static const uint8_t REPORT_TOUCH = 1;
static const uint8_t REPORT_KEYBOARD = 2;
static const uint8_t REPORT_CONSUMER = 3;

// flash.sh 覆盖 DKNM 之后的 20 字节。最后一字节保持 0，避免名称写满时没有结束符。
// DKNM + 20 字节名称 + 结束符。默认 DeekeScript-0000，横线后 4 位。
__attribute__((used)) static const uint8_t kNameBlob[25] = {
    'D', 'K', 'N', 'M', 'D', 'e', 'e', 'k', 'e', 'S', 'c', 'r', 'i', 'p', 't', '-', '0', '0', '0', '0', 0, 0, 0, 0, 0};

static const char* deviceName() {
    return reinterpret_cast<const char*>(kNameBlob + 4);
}

extern "C" int ble_svc_gap_device_appearance_set(uint16_t appearance);

// 触摸屏在最前。接触编号让内核设置 INPUT_PROP_DIRECT，手机就不再显示鼠标箭头。
// 不写 Physical Max，避免右侧多出一条区域。
static uint8_t REPORT_MAP[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, REPORT_TOUCH, 0x09, 0x22, 0xA1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0x09, 0x51, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xC0,
    0x05, 0x0D, 0x09, 0x54, 0x25, 0x01, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0,

    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, REPORT_KEYBOARD, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x73, 0x05, 0x07, 0x19, 0x00, 0x29, 0x73, 0x81, 0x00, 0xC0,

    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, REPORT_CONSUMER, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF,
    0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
};

static QueueHandle_t gQueue = nullptr;
static NimBLECharacteristic* gTouch = nullptr;
static NimBLECharacteristic* gKeyboard = nullptr;
static NimBLECharacteristic* gConsumer = nullptr;
static NimBLECharacteristic* gData = nullptr;
static NimBLEHIDDevice* gHid = nullptr;

static int gScreenW = 1080;
static int gScreenH = 1920;
static int gLastX = 0;
static int gLastY = 0;
static bool gFinger = false;
static uint16_t gRndLo = 30;
static uint16_t gRndHi = 30;
static uint8_t gMods = 0;
static uint8_t gKeys[6] = {0, 0, 0, 0, 0, 0};
static uint8_t gLastRaw[160];
static uint16_t gLastRawLen = 0;

static uint16_t ru16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

// 触摸屏横轴对宽度、纵轴对高度，各自映射到 0..32767。
static uint16_t scalePixel(int v, int size) {
    if (size <= 1) {
        return 0;
    }
    if (v < 0) {
        v = 0;
    }
    if (v >= size) {
        v = size - 1;
    }
    return (uint16_t)(((uint32_t)v * 32767u) / (uint32_t)size);
}

static void sendAck(uint8_t cmd, uint8_t status, const uint8_t* extra, uint16_t extraLen) {
    if (gData == nullptr) {
        return;
    }
    uint8_t buf[183];
    buf[0] = 0xA0;
    buf[1] = cmd;
    buf[2] = status;
    if (extraLen > 160) {
        extraLen = 160;
    }
    if (extra != nullptr && extraLen > 0) {
        memcpy(buf + 3, extra, extraLen);
    }
    gData->notify(buf, (size_t)(3 + extraLen));
}

// 7 字节：指尖、接触编号、X、Y、接触数量。按下时数量为 1，抬起时为 0。
static void sendTouch(uint8_t flags, int x, int y) {
    if (gTouch == nullptr) {
        return;
    }
    gLastX = x;
    gLastY = y;
    uint16_t hx = scalePixel(x, gScreenW);
    uint16_t hy = scalePixel(y, gScreenH);
    uint8_t down = (flags & 0x01) ? 1 : 0;
    uint8_t m[7] = {down,
                    0,
                    (uint8_t)(hx & 0xff),
                    (uint8_t)(hx >> 8),
                    (uint8_t)(hy & 0xff),
                    (uint8_t)(hy >> 8),
                    down};
    bool ok = gTouch->notify(m, sizeof(m));
    if (down && !ok) {
        Serial.println("touch notify failed");
    }
}

static void fingerDown(int x, int y) {
    gFinger = true;
    sendTouch(0x01, x, y);
}

static void fingerMove(int x, int y) {
    gFinger = true;
    sendTouch(0x01, x, y);
}

static void fingerUp() {
    gFinger = false;
    sendTouch(0x00, gLastX, gLastY);
}

static void sendKeyboard() {
    if (gKeyboard == nullptr) {
        return;
    }
    uint8_t r[8] = {gMods, 0, gKeys[0], gKeys[1], gKeys[2], gKeys[3], gKeys[4], gKeys[5]};
    gKeyboard->notify(r, sizeof(r));
}

static void keyDown(uint8_t usage) {
    for (uint8_t k : gKeys) {
        if (k == usage) {
            sendKeyboard();
            return;
        }
    }
    for (uint8_t& k : gKeys) {
        if (k == 0) {
            k = usage;
            sendKeyboard();
            return;
        }
    }
}

static void keyUp(uint8_t usage) {
    for (uint8_t& k : gKeys) {
        if (k == usage) {
            k = 0;
        }
    }
    sendKeyboard();
}

static void keyUpAll() {
    gMods = 0;
    memset(gKeys, 0, sizeof(gKeys));
    sendKeyboard();
    if (gConsumer != nullptr) {
        uint8_t z[2] = {0, 0};
        gConsumer->notify(z, sizeof(z));
    }
}

static void keyPress(uint8_t usage) {
    keyDown(usage);
    delay(30);
    keyUp(usage);
}

static void combo(uint8_t mod, uint8_t usage) {
    uint8_t saved = gMods;
    gMods = (uint8_t)(gMods | mod);
    keyDown(usage);
    delay(30);
    keyUp(usage);
    gMods = saved;
    sendKeyboard();
}

static void consumer(uint16_t usage) {
    if (gConsumer == nullptr) {
        return;
    }
    uint8_t r[2] = {(uint8_t)(usage & 0xff), (uint8_t)(usage >> 8)};
    gConsumer->notify(r, sizeof(r));
}

static void consumerTap(uint16_t usage, uint16_t ms) {
    consumer(usage);
    delay(ms == 0 ? 40 : ms);
    consumer(0);
}

static int tapDelay() {
    if (gRndHi <= gRndLo) {
        return gRndLo;
    }
    return gRndLo + (int)(esp_random() % (uint32_t)(gRndHi - gRndLo + 1));
}

static void runPath(int x1, int y1, int x2, int y2, int radian, int step, int downMs, int upMs, int gap, int times) {
    if (step < 1) {
        step = 1;
    }
    if (times < 1) {
        times = 1;
    }
    float x1f = x1, y1f = y1, x2f = x2, y2f = y2;
    float dx = x2f - x1f, dy = y2f - y1f;
    float len = sqrtf(dx * dx + dy * dy);
    float cx = (x1f + x2f) / 2.0f;
    float cy = (y1f + y2f) / 2.0f;
    if (len > 1.0f && radian != 0) {
        cx += (-dy / len) * (float)radian;
        cy += (dx / len) * (float)radian;
    }
    int steps = 1;
    if (len > step) {
        steps = (int)(len / (float)step);
    }
    if (steps > 64) {
        steps = 64;
    }
    fingerDown(x1, y1);
    if (downMs > 0) {
        delay(downMs);
    }
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float u = 1.0f - t;
        int x = (int)(u * u * x1f + 2 * u * t * cx + t * t * x2f);
        int y = (int)(u * u * y1f + 2 * u * t * cy + t * t * y2f);
        fingerMove(x, y);
        if (gap > 0) {
            delay(gap);
        }
    }
    if (upMs > 0) {
        delay(upMs);
    }
    fingerUp();
    for (int i = 1; i < times; i++) {
        fingerDown(x2, y2);
        delay(30);
        fingerUp();
    }
}

static void handle(const Cmd& cmd) {
    if (cmd.len < 1) {
        return;
    }
    const uint8_t op = cmd.data[0];
    const uint8_t* p = cmd.data;
    auto need = [&](uint16_t n) { return cmd.len >= n; };
    switch (op) {
        case CMD_TAP:
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            fingerDown(ru16(p + 1), ru16(p + 3));
            delay(tapDelay());
            fingerUp();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_DOWN:
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            fingerDown(ru16(p + 1), ru16(p + 3));
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_MOVE:
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            fingerMove(ru16(p + 1), ru16(p + 3));
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_UP:
            fingerUp();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_UP_XY:
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            gLastX = ru16(p + 1);
            gLastY = ru16(p + 3);
            fingerUp();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_SWIPE:
            if (!need(16)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            runPath(ru16(p + 1), ru16(p + 3), ru16(p + 5), ru16(p + 7), 0, p[9], ru16(p + 10), ru16(p + 12), p[14],
                    p[15]);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_SWIPEX:
            if (!need(18)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            runPath(ru16(p + 1), ru16(p + 3), ru16(p + 5), ru16(p + 7), ru16(p + 9), p[11], ru16(p + 12), ru16(p + 14),
                    p[16], p[17]);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_KEY_DOWN:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            keyDown(p[1]);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_KEY_UP:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            keyUp(p[1]);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_KEY_PRESS:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            keyPress(p[1]);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_KEY_UP_ALL:
            keyUpAll();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_COMBO:
            if (!need(3)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            combo(p[1], p[2]);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_CONSUMER:
            if (!need(3)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            consumerTap(ru16(p + 1), 40);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_MOD_DOWN:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            gMods = (uint8_t)(gMods | p[1]);
            sendKeyboard();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_MOD_UP:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            gMods = (uint8_t)(gMods & (uint8_t)~p[1]);
            sendKeyboard();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_CONSUMER_HOLD:
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            consumerTap(ru16(p + 1), ru16(p + 3));
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_CONSUMER_DOWN:
            if (!need(3)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            consumer(ru16(p + 1));
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_CONSUMER_UP:
            consumer(0);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_SET_XY:
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            gScreenW = ru16(p + 1);
            gScreenH = ru16(p + 3);
            if (gScreenW < 2) {
                gScreenW = 2;
            }
            if (gScreenH < 2) {
                gScreenH = 2;
            }
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_SET_RND: {
            if (!need(5)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            uint16_t a = ru16(p + 1);
            uint16_t b = ru16(p + 3);
            if (a > b) {
                uint16_t t = a;
                a = b;
                b = t;
            }
            gRndLo = a;
            gRndHi = b;
            sendAck(op, 0, nullptr, 0);
            return;
        }
        case CMD_SET_BAT:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            if (gHid != nullptr) {
                uint8_t lv = p[1];
                if (lv > 100) {
                    lv = 100;
                }
                gHid->setBatteryLevel(lv, true);
            }
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_REG: {
            size_t n = cmd.len > 1 ? (size_t)(cmd.len - 1) : 0;
            if (n > 160) {
                n = 160;
            }
            Preferences prefs;
            prefs.begin("deeke", false);
            size_t wrote = n == 0 ? 0 : prefs.putBytes("reg", p + 1, n);
            prefs.end();
            sendAck(op, (n == 0 || wrote == n) ? 0 : 1, nullptr, 0);
            return;
        }
        case CMD_REBOOT:
            sendAck(op, 0, nullptr, 0);
            delay(120);
            ESP.restart();
            return;
        case CMD_VER: {
            uint8_t ver[2] = {(uint8_t)(FW_VER & 0xff), (uint8_t)(FW_VER >> 8)};
            sendAck(op, 0, ver, 2);
            return;
        }
        case CMD_GET:
            sendAck(op, 0, gLastRaw, gLastRawLen);
            return;
        case CMD_RAW: {
            uint16_t n = (uint16_t)(cmd.len > 1 ? cmd.len - 1 : 0);
            if (n > sizeof(gLastRaw)) {
                n = sizeof(gLastRaw);
            }
            if (n > 0) {
                memcpy(gLastRaw, p + 1, n);
            }
            gLastRawLen = n;
            sendAck(op, 0, gLastRaw, gLastRawLen);
            return;
        }
        default:
            sendAck(op, 1, nullptr, 0);
            return;
    }
}

static void hidTask(void*) {
    Cmd cmd;
    for (;;) {
        if (xQueueReceive(gQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            handle(cmd);
        }
    }
}

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        NimBLEAttValue value = c->getValue();
        if (value.size() == 0 || value.size() > sizeof(Cmd::data)) {
            return;
        }
        Cmd cmd;
        cmd.len = value.size();
        memcpy(cmd.data, value.data(), value.size());
        if (gQueue == nullptr || xQueueSend(gQueue, &cmd, 0) != pdTRUE) {
            sendAck(cmd.data[0], 2, nullptr, 0);
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        Serial.printf("connected %s\n", info.getAddress().toString().c_str());
        int mtuRc = ble_gattc_exchange_mtu(info.getConnHandle(), nullptr, nullptr);
        Serial.printf("mtu exchange %d\n", mtuRc);
        NimBLEDevice::startSecurity(info.getConnHandle());
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("disconnected %d\n", reason);
        gFinger = false;
        keyUpAll();
        NimBLEDevice::startAdvertising();
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        Serial.printf("paired encrypted=%d\n", info.isEncrypted() ? 1 : 0);
    }
};

static CmdCallbacks gCmdCallbacks;
static ServerCallbacks gServerCallbacks;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("DeekeHid name=%s fw=%u map=%u\n", deviceName(), FW_VER, (unsigned)sizeof(REPORT_MAP));

    gQueue = xQueueCreate(8, sizeof(Cmd));
    xTaskCreate(hidTask, "deeke-hid", 8192, nullptr, 2, nullptr);

    NimBLEDevice::init(deviceName());
    ble_svc_gap_device_appearance_set(HID_KEYBOARD);
    NimBLEDevice::setMTU(185);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&gServerCallbacks);

    gHid = new NimBLEHIDDevice(server);
    gHid->setReportMap(REPORT_MAP, sizeof(REPORT_MAP));
    gHid->setManufacturer("DeekeScript");
    gHid->setPnp(0x02, 0x303A, 0x1007, FW_VER);
    gHid->setHidInfo(0x00, 0x02);
    gHid->setBatteryLevel(100);
    gTouch = gHid->getInputReport(REPORT_TOUCH);
    gKeyboard = gHid->getInputReport(REPORT_KEYBOARD);
    gConsumer = gHid->getInputReport(REPORT_CONSUMER);

    NimBLEService* svc = server->createService(CMD_SERVICE_UUID);
    NimBLECharacteristic* cmd = svc->createCharacteristic(
        CMD_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    cmd->setCallbacks(&gCmdCallbacks);
    gData = svc->createCharacteristic(DATA_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName(deviceName());
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(gHid->getHidService()->getUUID());
    NimBLEAdvertisementData scan;
    scan.setName(deviceName());
    scan.setAppearance(HID_KEYBOARD);
    scan.addServiceUUID(svc->getUUID());
    adv->setScanResponseData(scan);
    adv->enableScanResponse(true);
    adv->start();
    Serial.println("advertising");
}

void loop() {
    delay(1000);
}
