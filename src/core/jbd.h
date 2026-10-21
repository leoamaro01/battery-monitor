// JBD BMS BLE protocol: request building, reply reassembly and parsing.
// Pure C++ with no platform dependencies, so it runs in the simulator and on the ESP32.
#pragma once

#include <cstddef>
#include <cstdint>

namespace jbd {

constexpr const char *SERVICE_UUID = "ff00";
constexpr const char *NOTIFY_UUID = "ff01";
constexpr const char *WRITE_UUID = "ff02";

constexpr uint8_t CMD_BASIC_INFO = 0x03;
constexpr uint8_t CMD_CELL_VOLTAGES = 0x04;

constexpr int MAX_CELLS = 16;
constexpr int MAX_TEMPS = 4;
constexpr size_t MAX_PAYLOAD = 64;

extern const char *const PROTECTION_FLAGS[13];

struct BasicInfo {
    float voltage;       // V
    float current;       // A, negative = discharging
    float remainingAh;
    float nominalAh;
    float fullChargeAh;  // 0 when the firmware doesn't report it
    uint16_t cycles;
    uint16_t year;
    uint8_t month, day;
    uint32_t balancing;  // bitmask of cells being balanced
    uint16_t protection; // bitmask, see PROTECTION_FLAGS
    uint8_t soc;         // %
    bool chargeFet;
    bool dischargeFet;
    uint8_t cellCount;
    uint8_t tempCount;
    float temps[MAX_TEMPS]; // °C

    float capacityAh() const { return fullChargeAh > 0 ? fullChargeAh : nominalAh; }
};

// Writes the 7-byte read request for `cmd` into `out`.
void buildRequest(uint8_t cmd, uint8_t out[7]);

bool parseBasicInfo(const uint8_t *payload, size_t len, BasicInfo &out);
// Returns the number of cells written to `out` (volts).
int parseCells(const uint8_t *payload, size_t len, float *out, int maxCells);

// Reassembles a reply that arrives split across several BLE notifications.
class FrameAssembler {
  public:
    // Feed one notification. Returns true when a complete, valid frame is ready;
    // read it with cmd()/payload()/payloadLen() before feeding more data.
    bool feed(const uint8_t *data, size_t len);
    void reset() { len_ = 0; }

    uint8_t cmd() const { return frameCmd_; }
    const uint8_t *payload() const { return frame_; }
    size_t payloadLen() const { return frameLen_; }

  private:
    bool extract();

    uint8_t buf_[128];
    size_t len_ = 0;
    uint8_t frameCmd_ = 0;
    uint8_t frame_[MAX_PAYLOAD];
    size_t frameLen_ = 0;
};

} // namespace jbd
