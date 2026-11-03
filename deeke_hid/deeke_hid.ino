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
 * 最多同时连 3 台手机。每台的点击、按键和屏幕尺寸只发给发指令的那一台。
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include "hid_types.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"

#include <cmath>
#include <cstring>

static const uint16_t FW_VER = 14;
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

    // vivo 会丢掉 0..1023 这一大段数组。报告值是用途列表里的下标，0 必须空着，否则 6 会落到 Home。
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, REPORT_CONSUMER, 0x15, 0x00, 0x25, 0x08, 0x09, 0x00, 0x09, 0x30, 0x09,
    0x40, 0x09, 0xE9, 0x09, 0xEA, 0x0A, 0xA2, 0x01, 0x0A, 0x1D, 0x02, 0x0A, 0x23, 0x02, 0x0A, 0x24, 0x02, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x00, 0xC0,
};

static const int kMaxPeers = 3;
static QueueHandle_t gQueue = nullptr;
static QueueHandle_t gLinkQueue = nullptr;

static const uint8_t LINK_ADV = 1;
static const uint8_t LINK_STOP = 2;
static const uint8_t LINK_PARAMS = 3;

struct LinkCmd {
    uint8_t op;
    uint16_t handle;
};

// 触摸、键盘、消费键、指令通知。预编译协议栈只能持久化 8 条订阅，3 台手机会超过。
static uint16_t gKeepCccd[4];
static bool gAcceptUnstoredCccd = false;
static ble_store_write_fn* gRealStoreWrite = nullptr;

static bool keepCccd(uint16_t handle) {
    if (handle == 0) {
        return false;
    }
    for (uint16_t keep : gKeepCccd) {
        if (keep == handle) {
            return true;
        }
    }
    return false;
}

// 存不下时不要让手机的订阅写入失败。失败的话系统会关掉这条通知，消费键就点不出去。
static int storeWriteCccd(int obj_type, const union ble_store_value* val) {
    if (gAcceptUnstoredCccd && obj_type == BLE_STORE_OBJ_TYPE_CCCD && val != nullptr &&
        keepCccd(val->cccd.chr_val_handle)) {
        gAcceptUnstoredCccd = false;
        return 0;
    }
    if (gRealStoreWrite == nullptr) {
        return BLE_HS_ENOTSUP;
    }
    return gRealStoreWrite(obj_type, val);
}

struct CccdKeep {
    ble_addr_t addr;
    uint16_t handle;
    bool dropped;
};

extern "C" int dropOneCccd(int, union ble_store_value* value, void* cookie) {
    auto* keep = static_cast<CccdKeep*>(cookie);
    if (keep->dropped) {
        return 1;
    }
    const ble_store_value_cccd& cccd = value->cccd;
    if (ble_addr_cmp(&cccd.peer_addr, &keep->addr) == 0 && cccd.chr_val_handle == keep->handle) {
        return 0;
    }
    for (uint16_t handle : gKeepCccd) {
        if (handle != 0 && handle == cccd.chr_val_handle) {
            return 0;
        }
    }
    ble_store_key_cccd key{};
    ble_store_key_from_value_cccd(&key, &cccd);
    if (ble_store_delete_cccd(&key) == 0) {
        keep->dropped = true;
        Serial.println("drop old cccd");
        return 1;
    }
    return 0;
}

static void tuneLink(uint16_t handle) {
    ble_gap_upd_params params{};
    params.itvl_min = 24;
    params.itvl_max = 24;
    params.latency = 0;
    params.supervision_timeout = 400;
    params.min_ce_len = 0;
    params.max_ce_len = 0;
    int rc = ble_gap_update_params(handle, &params);
    Serial.printf("params %u rc=%d\n", handle, rc);
}

static void postLink(uint8_t op, uint16_t handle) {
    if (gLinkQueue == nullptr) {
        return;
    }
    LinkCmd cmd{op, handle};
    xQueueSend(gLinkQueue, &cmd, 0);
}

class StoreCallbacks : public NimBLEDeviceCallbacks {
    int onStoreStatus(struct ble_store_status_event* event, void* arg) override {
        if (event != nullptr && event->event_code == BLE_STORE_EVENT_OVERFLOW &&
            event->overflow.obj_type == BLE_STORE_OBJ_TYPE_CCCD && event->overflow.value != nullptr) {
            CccdKeep keep{};
            keep.addr = event->overflow.value->cccd.peer_addr;
            keep.handle = event->overflow.value->cccd.chr_val_handle;
            ble_store_iterate(BLE_STORE_OBJ_TYPE_CCCD, dropOneCccd, &keep);
            if (keep.dropped) {
                return 0;
            }
            if (keepCccd(keep.handle)) {
                Serial.printf("cccd live h=%u\n", keep.handle);
                gAcceptUnstoredCccd = true;
                return 0;
            }
            return BLE_HS_ESTORE_CAP;
        }
        return ble_store_util_status_rr(event, arg);
    }
};

static StoreCallbacks gStoreCallbacks;

static void serviceLinks() {
    if (gLinkQueue == nullptr) {
        return;
    }
    NimBLEServer* server = NimBLEDevice::getServer();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (server == nullptr || adv == nullptr) {
        return;
    }
    LinkCmd cmd;
    while (xQueueReceive(gLinkQueue, &cmd, 0) == pdTRUE) {
        if (cmd.op == LINK_PARAMS) {
            tuneLink(cmd.handle);
            continue;
        }
        uint8_t peers = server->getConnectedCount();
        if (cmd.op == LINK_STOP || peers >= kMaxPeers) {
            if (adv->isAdvertising()) {
                adv->stop();
            }
            Serial.printf("adv stop peers=%u\n", peers);
            continue;
        }
        adv->setMinInterval(32);
        adv->setMaxInterval(64);
        bool ok = adv->isAdvertising() || adv->start();
        Serial.printf("adv %s peers=%u\n", ok ? "on" : "fail", peers);
        if (!ok) {
            xQueueSend(gLinkQueue, &cmd, 0);
            break;
        }
    }
}
static NimBLECharacteristic* gTouch = nullptr;
static NimBLECharacteristic* gKeyboard = nullptr;
static NimBLECharacteristic* gConsumer = nullptr;
static NimBLECharacteristic* gData = nullptr;
static NimBLEHIDDevice* gHid = nullptr;

static Peer gPeers[kMaxPeers];
static Peer* gCur = nullptr;

class ReportSubCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        const char* name = "data";
        if (chr == gTouch) {
            name = "touch";
        } else if (chr == gKeyboard) {
            name = "key";
        } else if (chr == gConsumer) {
            name = "consumer";
        }
        Serial.printf("sub %s conn=%u on=%u\n", name, info.getConnHandle(), subValue);
    }
};

static ReportSubCallbacks gReportSubCallbacks;

static Peer* peerFor(uint16_t handle) {
    Peer* freeSlot = nullptr;
    for (int i = 0; i < kMaxPeers; i++) {
        if (gPeers[i].used && gPeers[i].handle == handle) {
            return &gPeers[i];
        }
        if (!gPeers[i].used && freeSlot == nullptr) {
            freeSlot = &gPeers[i];
        }
    }
    if (freeSlot == nullptr) {
        return nullptr;
    }
    memset(freeSlot, 0, sizeof(Peer));
    freeSlot->used = true;
    freeSlot->handle = handle;
    freeSlot->screenW = 1080;
    freeSlot->screenH = 1920;
    freeSlot->rndLo = 30;
    freeSlot->rndHi = 30;
    return freeSlot;
}

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

static void sendAckTo(uint16_t conn, uint8_t cmd, uint8_t status, const uint8_t* extra, uint16_t extraLen) {
    if (gData == nullptr || conn == BLE_HS_CONN_HANDLE_NONE) {
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
    gData->notify(buf, (size_t)(3 + extraLen), conn);
}

static void sendAck(uint8_t cmd, uint8_t status, const uint8_t* extra, uint16_t extraLen) {
    if (gCur == nullptr) {
        return;
    }
    sendAckTo(gCur->handle, cmd, status, extra, extraLen);
}

// 7 字节：指尖、接触编号、X、Y、接触数量。按下时数量为 1，抬起时为 0。
static void sendTouch(uint8_t flags, int x, int y) {
    if (gTouch == nullptr || gCur == nullptr) {
        return;
    }
    gCur->lastX = x;
    gCur->lastY = y;
    uint16_t hx = scalePixel(x, gCur->screenW);
    uint16_t hy = scalePixel(y, gCur->screenH);
    uint8_t down = (flags & 0x01) ? 1 : 0;
    uint8_t m[7] = {down,
                    0,
                    (uint8_t)(hx & 0xff),
                    (uint8_t)(hx >> 8),
                    (uint8_t)(hy & 0xff),
                    (uint8_t)(hy >> 8),
                    down};
    bool ok = gTouch->notify(m, sizeof(m), gCur->handle);
    if (down && !ok) {
        Serial.println("touch notify failed");
    }
}

static void fingerDown(int x, int y) {
    gCur->finger = true;
    sendTouch(0x01, x, y);
}

static void fingerMove(int x, int y) {
    gCur->finger = true;
    sendTouch(0x01, x, y);
}

static void fingerUp() {
    gCur->finger = false;
    sendTouch(0x00, gCur->lastX, gCur->lastY);
}

static void sendKeyboard() {
    if (gKeyboard == nullptr || gCur == nullptr) {
        return;
    }
    uint8_t r[8] = {gCur->mods, 0, gCur->keys[0], gCur->keys[1], gCur->keys[2], gCur->keys[3], gCur->keys[4], gCur->keys[5]};
    gKeyboard->notify(r, sizeof(r), gCur->handle);
}

static void keyDown(uint8_t usage) {
    for (uint8_t k : gCur->keys) {
        if (k == usage) {
            sendKeyboard();
            return;
        }
    }
    for (uint8_t& k : gCur->keys) {
        if (k == 0) {
            k = usage;
            sendKeyboard();
            return;
        }
    }
}

static void keyUp(uint8_t usage) {
    for (uint8_t& k : gCur->keys) {
        if (k == usage) {
            k = 0;
        }
    }
    sendKeyboard();
}

static void keyUpAll() {
    gCur->mods = 0;
    memset(gCur->keys, 0, sizeof(gCur->keys));
    sendKeyboard();
    if (gConsumer != nullptr && gCur != nullptr) {
        uint8_t z = 0;
        gConsumer->notify(&z, 1, gCur->handle);
    }
}

static void keyPress(uint8_t usage) {
    keyDown(usage);
    delay(30);
    keyUp(usage);
}

static void combo(uint8_t mod, uint8_t usage) {
    uint8_t saved = gCur->mods;
    gCur->mods = (uint8_t)(gCur->mods | mod);
    keyDown(usage);
    delay(30);
    keyUp(usage);
    gCur->mods = saved;
    sendKeyboard();
}

static uint8_t consumerIndex(uint16_t usage) {
    switch (usage) {
        case 0x0030:
            return 1;
        case 0x0040:
            return 2;
        case 0x00E9:
            return 3;
        case 0x00EA:
            return 4;
        case 0x01A2:
            return 5;
        case 0x021D:
            return 6;
        case 0x0223:
            return 7;
        case 0x0224:
            return 8;
        default:
            return 0;
    }
}

static void consumer(uint16_t usage) {
    if (gConsumer == nullptr || gCur == nullptr) {
        return;
    }
    uint8_t index = consumerIndex(usage);
    if (usage != 0 && index == 0) {
        Serial.printf("consumer usage %u unmapped\n", usage);
    }
    if (!gConsumer->notify(&index, 1, gCur->handle)) {
        Serial.printf("consumer notify fail usage=%u conn=%u chr=%u\n", usage, gCur->handle, gConsumer->getHandle());
    }
}

static void consumerTap(uint16_t usage, uint16_t ms) {
    consumer(usage);
    delay(ms == 0 ? 100 : ms);
    consumer(0);
}

static int tapDelay() {
    if (gCur->rndHi <= gCur->rndLo) {
        return gCur->rndLo;
    }
    return gCur->rndLo + (int)(esp_random() % (uint32_t)(gCur->rndHi - gCur->rndLo + 1));
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

static void releasePeer(uint16_t handle) {
    for (int i = 0; i < kMaxPeers; i++) {
        if (!gPeers[i].used || gPeers[i].handle != handle) {
            continue;
        }
        gCur = &gPeers[i];
        if (gCur->finger) {
            fingerUp();
        }
        keyUpAll();
        gPeers[i].used = false;
        gCur = nullptr;
        return;
    }
}

static void handle(const Cmd& cmd) {
    if (cmd.drop) {
        releasePeer(cmd.conn);
        return;
    }
    gCur = peerFor(cmd.conn);
    if (gCur == nullptr || cmd.len < 1) {
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
            gCur->lastX = ru16(p + 1);
            gCur->lastY = ru16(p + 3);
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
            consumerTap(ru16(p + 1), 100);
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_MOD_DOWN:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            gCur->mods = (uint8_t)(gCur->mods | p[1]);
            sendKeyboard();
            sendAck(op, 0, nullptr, 0);
            return;
        case CMD_MOD_UP:
            if (!need(2)) {
                sendAck(op, 1, nullptr, 0);
                return;
            }
            gCur->mods = (uint8_t)(gCur->mods & (uint8_t)~p[1]);
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
            gCur->screenW = ru16(p + 1);
            gCur->screenH = ru16(p + 3);
            if (gCur->screenW < 2) {
                gCur->screenW = 2;
            }
            if (gCur->screenH < 2) {
                gCur->screenH = 2;
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
            gCur->rndLo = a;
            gCur->rndHi = b;
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
            sendAck(op, 0, gCur->lastRaw, gCur->lastRawLen);
            return;
        case CMD_RAW: {
            uint16_t n = (uint16_t)(cmd.len > 1 ? cmd.len - 1 : 0);
            if (n > sizeof(gCur->lastRaw)) {
                n = sizeof(gCur->lastRaw);
            }
            if (n > 0) {
                memcpy(gCur->lastRaw, p + 1, n);
            }
            gCur->lastRawLen = n;
            sendAck(op, 0, gCur->lastRaw, gCur->lastRawLen);
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
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        NimBLEAttValue value = c->getValue();
        if (value.size() == 0 || value.size() > sizeof(Cmd::data)) {
            return;
        }
        Cmd cmd{};
        cmd.conn = info.getConnHandle();
        cmd.len = value.size();
        memcpy(cmd.data, value.data(), value.size());
        if (gQueue == nullptr || xQueueSend(gQueue, &cmd, 0) != pdTRUE) {
            sendAckTo(cmd.conn, cmd.data[0], 2, nullptr, 0);
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        Serial.printf("connected %s count=%u\n", info.getAddress().toString().c_str(), server->getConnectedCount());
        int mtuRc = ble_gattc_exchange_mtu(info.getConnHandle(), nullptr, nullptr);
        Serial.printf("mtu exchange %d\n", mtuRc);
        NimBLEDevice::startSecurity(info.getConnHandle());
        postLink(server->getConnectedCount() < kMaxPeers ? LINK_ADV : LINK_STOP, 0);
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        Serial.printf("disconnected %d\n", reason);
        Cmd cmd{};
        cmd.drop = 1;
        cmd.conn = info.getConnHandle();
        if (gQueue != nullptr) {
            xQueueSend(gQueue, &cmd, 0);
        }
        postLink(LINK_ADV, 0);
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        Serial.printf("paired encrypted=%d\n", info.isEncrypted() ? 1 : 0);
        if (info.isEncrypted()) {
            postLink(LINK_PARAMS, info.getConnHandle());
        }
    }
};

static CmdCallbacks gCmdCallbacks;
static ServerCallbacks gServerCallbacks;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("DeekeHid name=%s fw=%u map=%u\n", deviceName(), FW_VER, (unsigned)sizeof(REPORT_MAP));

    gQueue = xQueueCreate(24, sizeof(Cmd));
    gLinkQueue = xQueueCreate(8, sizeof(LinkCmd));
    xTaskCreate(hidTask, "deeke-hid", 8192, nullptr, 2, nullptr);

    NimBLEDevice::setDeviceCallbacks(&gStoreCallbacks);
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
    gTouch->setCallbacks(&gReportSubCallbacks);
    gKeyboard->setCallbacks(&gReportSubCallbacks);
    gConsumer->setCallbacks(&gReportSubCallbacks);

    NimBLEService* svc = server->createService(CMD_SERVICE_UUID);
    NimBLECharacteristic* cmd = svc->createCharacteristic(
        CMD_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
    cmd->setCallbacks(&gCmdCallbacks);
    gData = svc->createCharacteristic(DATA_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    gData->setCallbacks(&gReportSubCallbacks);

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setMinInterval(32);
    adv->setMaxInterval(64);
    bool hidOk = adv->addServiceUUID(gHid->getHidService()->getUUID());
    bool prefOk = adv->setPreferredParams(24, 24);
    bool nameOk = adv->setName(deviceName());
    NimBLEAdvertisementData scan;
    scan.setName(deviceName());
    scan.setAppearance(HID_KEYBOARD);
    bool scanOk = adv->setScanResponseData(scan);
    adv->enableScanResponse(true);
    bool started = adv->start();
    gKeepCccd[0] = gTouch->getHandle();
    gKeepCccd[1] = gKeyboard->getHandle();
    gKeepCccd[2] = gConsumer->getHandle();
    gKeepCccd[3] = gData->getHandle();
    if (gRealStoreWrite == nullptr) {
        gRealStoreWrite = ble_hs_cfg.store_write_cb;
        ble_hs_cfg.store_write_cb = storeWriteCccd;
    }
    Serial.printf("adv start=%d hid=%d pref=%d name=%d scan=%d bytes=%u cccd=%u,%u,%u,%u\n", started ? 1 : 0,
                  hidOk ? 1 : 0, prefOk ? 1 : 0, nameOk ? 1 : 0, scanOk ? 1 : 0,
                  (unsigned)adv->getAdvertisementData().getPayload().size(), gKeepCccd[0], gKeepCccd[1], gKeepCccd[2],
                  gKeepCccd[3]);
}

void loop() {
    serviceLinks();
    static uint32_t lastAdvCheck = 0;
    uint32_t now = millis();
    if (now - lastAdvCheck >= 1000) {
        lastAdvCheck = now;
        NimBLEServer* server = NimBLEDevice::getServer();
        NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
        if (server != nullptr && adv != nullptr && server->getConnectedCount() < kMaxPeers && !adv->isAdvertising()) {
            postLink(LINK_ADV, 0);
        }
    }
    delay(200);
}
