#include "ble_batteries.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <strings.h>

#include "../../core/jbd.h"
#include "../../core/state.h"

namespace {

constexpr uint32_t SCAN_MS = 10000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t SETTLE_MS = 1000; // the BMS ignores commands right after connecting
constexpr uint32_t REPLY_TIMEOUT_MS = 2000;
constexpr int ATTEMPTS = 3;
constexpr int FAILURES_BEFORE_RECONNECT = 3;

const char *const CONFIGURED_ADDRESSES[BATTERY_COUNT] = BATTERY_ADDRESSES;

struct Link {
    int index;
    bool found = false;
    NimBLEAddress address;
    NimBLEClient *client = nullptr;
    NimBLERemoteCharacteristic *tx = nullptr;
    bool writeWithResponse = true;
    int failures = 0;

    jbd::FrameAssembler assembler;
    volatile uint8_t waitingFor = 0;
    SemaphoreHandle_t replied = xSemaphoreCreateBinary();
    uint8_t reply[jbd::MAX_PAYLOAD];
    size_t replyLen = 0;
};

Link links[BATTERY_COUNT];

void setLink(Link &l, LinkState s) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.batteries[l.index].link = s;
}

// Runs on the NimBLE host task.
void onNotify(Link &l, const uint8_t *data, size_t len) {
    if (!l.assembler.feed(data, len) || l.assembler.cmd() != l.waitingFor)
        return;
    l.replyLen = l.assembler.payloadLen();
    memcpy(l.reply, l.assembler.payload(), l.replyLen);
    l.waitingFor = 0;
    xSemaphoreGive(l.replied);
}

void scan() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    NimBLEScanResults results = scan->getResults(SCAN_MS, false);
    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice(i);
        std::string addr = dev->getAddress().toString();
        for (Link &l : links) {
            if (l.found)
                continue;
            const char *want = CONFIGURED_ADDRESSES[l.index];
            bool match = *want ? strcasecmp(addr.c_str(), want) == 0
                               : dev->getName().rfind(BATTERY_NAME_PREFIX, 0) == 0;
            // Don't let a name-matched slot grab a battery another slot is configured for.
            for (const char *other : CONFIGURED_ADDRESSES)
                if (!*want && *other && strcasecmp(addr.c_str(), other) == 0)
                    match = false;
            bool taken = false;
            for (const Link &o : links)
                taken |= o.found && o.address == dev->getAddress();
            if (!match || taken)
                continue;
            l.found = true;
            l.address = dev->getAddress();
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                BatteryState &b = g_state.batteries[l.index];
                snprintf(b.address, sizeof b.address, "%s", addr.c_str());
                b.link = LinkState::Connecting;
            }
            Serial.printf("[ble] battery %c = %s (%s)\n", 'A' + l.index, addr.c_str(), dev->getName().c_str());
            break;
        }
    }
    scan->clearResults();
}

bool connect(Link &l) {
    if (l.client && l.client->isConnected())
        return true;
    setLink(l, LinkState::Connecting);
    if (!l.client) {
        l.client = NimBLEDevice::createClient(l.address);
        l.client->setConnectTimeout(CONNECT_TIMEOUT_MS);
    }
    if (!l.client->connect(l.address))
        return false;

    NimBLERemoteService *svc = l.client->getService(jbd::SERVICE_UUID);
    NimBLERemoteCharacteristic *rx = svc ? svc->getCharacteristic(jbd::NOTIFY_UUID) : nullptr;
    l.tx = svc ? svc->getCharacteristic(jbd::WRITE_UUID) : nullptr;
    if (!rx || !l.tx) {
        Serial.printf("[ble] battery %c: JBD service not found\n", 'A' + l.index);
        l.client->disconnect();
        return false;
    }
    l.writeWithResponse = !l.tx->canWriteNoResponse();
    Link *lp = &l;
    if (!rx->subscribe(true, [lp](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { onNotify(*lp, d, n); })) {
        l.client->disconnect();
        return false;
    }
    delay(SETTLE_MS);
    return true;
}

bool request(Link &l, uint8_t cmd) {
    uint8_t frame[7];
    jbd::buildRequest(cmd, frame);
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        l.assembler.reset();
        xSemaphoreTake(l.replied, 0); // clear a stale give
        l.waitingFor = cmd;
        if (!l.tx->writeValue(frame, sizeof frame, l.writeWithResponse))
            continue;
        if (xSemaphoreTake(l.replied, pdMS_TO_TICKS(REPLY_TIMEOUT_MS)) == pdTRUE)
            return true;
    }
    l.waitingFor = 0;
    return false;
}

void poll(Link &l) {
    jbd::BasicInfo info;
    float cells[jbd::MAX_CELLS];
    int cellCount = 0;
    bool ok = connect(l) && request(l, jbd::CMD_BASIC_INFO) && jbd::parseBasicInfo(l.reply, l.replyLen, info) &&
              request(l, jbd::CMD_CELL_VOLTAGES) &&
              (cellCount = jbd::parseCells(l.reply, l.replyLen, cells, jbd::MAX_CELLS)) > 0;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    BatteryState &b = g_state.batteries[l.index];
    if (ok) {
        l.failures = 0;
        b.link = LinkState::Connected;
        b.valid = true;
        b.updatedMs = millis();
        b.info = info;
        b.cellCount = cellCount;
        memcpy(b.cells, cells, sizeof(float) * cellCount);
        return;
    }
    b.link = LinkState::Lost;
    // A connection that stops answering gets dropped so the next round starts fresh.
    if (++l.failures >= FAILURES_BEFORE_RECONNECT && l.client && l.client->isConnected())
        l.client->disconnect();
}

void task(void *) {
    NimBLEDevice::init("battery-monitor");
    for (;;) {
        uint32_t start = millis();
        bool missing = false;
        for (const Link &l : links)
            missing |= !l.found;
        if (missing)
            scan();
        for (Link &l : links)
            if (l.found)
                poll(l);
        uint32_t elapsed = millis() - start;
        if (elapsed < BATTERY_POLL_MS)
            delay(BATTERY_POLL_MS - elapsed);
    }
}

} // namespace

void ble_batteries_start() {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (int i = 0; i < BATTERY_COUNT; i++) {
            links[i].index = i;
            g_state.batteries[i].label = 'A' + i;
            g_state.batteries[i].link = LinkState::Searching;
        }
    }
    // Core 0 alongside the radio stack; LVGL runs in loop() on core 1.
    xTaskCreatePinnedToCore(task, "ble", 8192, nullptr, 1, nullptr, 0);
}
