// Everything the UI shows. Platform code writes it (under the lock), the UI reads a snapshot.
#pragma once

#include <cstdint>
#include <mutex>

#include "config.h"
#include "jbd.h"

enum class LinkState : uint8_t { Searching, Connecting, Connected, Lost };

struct BatteryState {
    char label = '?';
    char address[18] = "";
    LinkState link = LinkState::Searching;
    bool valid = false;          // info/cells hold at least one reading
    uint32_t updatedMs = 0;      // platform millis() of the last good reading
    jbd::BasicInfo info{};
    int cellCount = 0;
    float cells[jbd::MAX_CELLS]{};
};

// Pack-level warnings, computed from both batteries.
enum Warning : uint16_t {
    WARN_SOC_DRIFT = 1 << 0,
    WARN_BATTERY_IMBALANCE = 1 << 1,
    WARN_CELL_SPREAD = 1 << 2,
    WARN_CURRENT_MISMATCH = 1 << 3,
    WARN_PROTECTION = 1 << 4,
    WARN_STALE = 1 << 5,
};

struct PackSummary {
    bool valid = false; // all batteries have data
    float voltage = 0, current = 0, power = 0;
    float remainingAh = 0, capacityAh = 0, energyKwh = 0;
    int soc = 0, socDrift = 0;
    float cellMin = 0, cellMax = 0, cellSpread = 0, avgCellDrift = 0;
    float tempMax = 0;
    float hoursLeft = -1; // -1 when not discharging
    uint16_t warnings = 0;
};

struct WeatherState {
    bool valid = false;
    float tempC = 0, highC = 0, lowC = 0, windKmh = 0;
    int humidity = 0;
    int code = 0; // WMO weather code
    bool isDay = true;
};

struct AppState {
    BatteryState batteries[BATTERY_COUNT];
    WeatherState weather;
    bool wifiConnected = false;
    bool timeValid = false;
};

extern AppState g_state;
extern std::mutex g_stateMutex;

PackSummary summarizePack(const AppState &s, uint32_t nowMs);
