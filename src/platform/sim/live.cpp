#include "live.h"

#include <ArduinoJson.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "../../core/jbd.h"
#include "../../core/platform.h"
#include "../../core/state.h"
#include "../../core/weather.h"

namespace {

constexpr int WEATHER_RETRY_S = 60;

std::string run(const std::string &cmd) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p)
        return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0)
        out.append(buf, n);
    pclose(p);
    return out;
}

void weatherLoop() {
    for (;;) {
        char url[320];
        weather::buildUrl(url, sizeof url);
        std::string body = run(std::string("curl -fsS --max-time 15 '") + url + "'");
        WeatherState w;
        bool ok = weather::parse(body.data(), body.size(), w);
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.wifiConnected = ok;
            if (ok)
                g_state.weather = w;
        }
        printf("[live] weather %s\n", ok ? weather::describe(w.code) : "fetch failed");
        std::this_thread::sleep_for(ok ? std::chrono::milliseconds(WEATHER_REFRESH_MS)
                                       : std::chrono::milliseconds(WEATHER_RETRY_S * 1000));
    }
}

bool hexToBytes(const char *hex, uint8_t *out, size_t max, size_t &len) {
    len = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        if (len == max)
            return false;
        char byte[3] = {p[0], p[1], 0};
        out[len++] = uint8_t(strtoul(byte, nullptr, 16));
    }
    return true;
}

// One line from jbd_monitor.py --json: {"label", "address", "basic": hex|null, "cells": hex|null, "error"}.
void applyLine(const std::string &line) {
    JsonDocument doc;
    if (deserializeJson(doc, line))
        return;
    const char *label = doc["label"] | "?";
    int index = label[0] - 'A';
    if (index < 0 || index >= BATTERY_COUNT)
        return;

    jbd::BasicInfo info;
    float cells[jbd::MAX_CELLS];
    int cellCount = 0;
    uint8_t payload[jbd::MAX_PAYLOAD];
    size_t len;
    bool ok = doc["error"].isNull() && doc["basic"].is<const char *>() && doc["cells"].is<const char *>() &&
              hexToBytes(doc["basic"], payload, sizeof payload, len) && jbd::parseBasicInfo(payload, len, info) &&
              hexToBytes(doc["cells"], payload, sizeof payload, len) &&
              (cellCount = jbd::parseCells(payload, len, cells, jbd::MAX_CELLS)) > 0;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    BatteryState &b = g_state.batteries[index];
    b.label = label[0];
    snprintf(b.address, sizeof b.address, "%s", doc["address"] | "");
    if (!ok) {
        b.link = b.valid ? LinkState::Lost : LinkState::Connecting;
        printf("[live] battery %c: %s\n", b.label, doc["error"] | "bad data");
        return;
    }
    b.link = LinkState::Connected;
    b.valid = true;
    b.updatedMs = platform_millis();
    b.info = info;
    b.cellCount = cellCount;
    memcpy(b.cells, cells, sizeof(float) * cellCount);
}

void batteryLoop(std::string cmd) {
    printf("[live] starting: %s\n", cmd.c_str());
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) {
        printf("[live] could not start the battery bridge\n");
        return;
    }
    std::string line;
    int c;
    while ((c = fgetc(p)) != EOF) {
        if (c != '\n') {
            line += char(c);
            continue;
        }
        applyLine(line);
        line.clear();
    }
    printf("[live] battery bridge exited with status %d\n", pclose(p));
    std::lock_guard<std::mutex> lock(g_stateMutex);
    for (auto &b : g_state.batteries)
        b.link = LinkState::Lost;
}

} // namespace

void live_weather_start() { std::thread(weatherLoop).detach(); }

void live_batteries_start(const char *bridgeCommand) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (int i = 0; i < BATTERY_COUNT; i++) {
            g_state.batteries[i] = BatteryState{};
            g_state.batteries[i].label = 'A' + i;
        }
    }
    std::thread(batteryLoop, std::string(bridgeCommand)).detach();
}
