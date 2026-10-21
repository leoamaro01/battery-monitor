#include "state.h"

#include <algorithm>
#include <cmath>

AppState g_state;
std::mutex g_stateMutex;

PackSummary summarizePack(const AppState &s, uint32_t nowMs) {
    PackSummary p;
    for (const auto &b : s.batteries) {
        if (!b.valid || b.cellCount == 0)
            return p;
        if (nowMs - b.updatedMs > STALE_AFTER_MS)
            p.warnings |= WARN_STALE;
        if (b.info.protection)
            p.warnings |= WARN_PROTECTION;
    }

    float currentMin = INFINITY, currentMax = -INFINITY, avgCellMin = INFINITY, avgCellMax = -INFINITY;
    int socMax = 0;
    p.soc = 100;
    p.remainingAh = INFINITY;
    p.capacityAh = INFINITY;
    p.cellMin = INFINITY;
    p.cellMax = -INFINITY;
    p.tempMax = -INFINITY;

    for (const auto &b : s.batteries) {
        const auto &i = b.info;
        p.voltage += i.voltage;
        p.current += i.current / BATTERY_COUNT;
        currentMin = std::min(currentMin, i.current);
        currentMax = std::max(currentMax, i.current);
        // In series the weakest battery limits the pack.
        p.remainingAh = std::min(p.remainingAh, i.remainingAh);
        p.capacityAh = std::min(p.capacityAh, i.capacityAh());
        p.soc = std::min<int>(p.soc, i.soc);
        socMax = std::max<int>(socMax, i.soc);
        float sum = 0;
        for (int c = 0; c < b.cellCount; c++) {
            p.cellMin = std::min(p.cellMin, b.cells[c]);
            p.cellMax = std::max(p.cellMax, b.cells[c]);
            sum += b.cells[c];
        }
        avgCellMin = std::min(avgCellMin, sum / b.cellCount);
        avgCellMax = std::max(avgCellMax, sum / b.cellCount);
        for (int t = 0; t < i.tempCount; t++)
            p.tempMax = std::max(p.tempMax, i.temps[t]);
    }

    p.power = p.voltage * p.current;
    p.energyKwh = p.remainingAh * p.voltage / 1000;
    p.socDrift = socMax - p.soc;
    p.cellSpread = p.cellMax - p.cellMin;
    p.avgCellDrift = avgCellMax - avgCellMin;
    p.hoursLeft = p.current < -0.1f ? p.remainingAh / -p.current : -1;

    if (p.socDrift >= SOC_DRIFT_WARN)
        p.warnings |= WARN_SOC_DRIFT;
    if (p.avgCellDrift >= AVG_CELL_DRIFT_WARN)
        p.warnings |= WARN_BATTERY_IMBALANCE;
    if (p.cellSpread >= CELL_SPREAD_WARN)
        p.warnings |= WARN_CELL_SPREAD;
    if (currentMax - currentMin >= CURRENT_MISMATCH_WARN)
        p.warnings |= WARN_CURRENT_MISMATCH;
    p.valid = true;
    return p;
}
