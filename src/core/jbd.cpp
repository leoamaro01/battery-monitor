#include "jbd.h"

#include <cstring>

namespace jbd {

const char *const PROTECTION_FLAGS[13] = {
    "cell overvoltage",   "cell undervoltage",   "pack overvoltage",      "pack undervoltage",
    "charge overtemp",    "charge undertemp",    "discharge overtemp",    "discharge undertemp",
    "charge overcurrent", "discharge overcurrent", "short circuit",       "frontend IC error",
    "MOSFET software lock",
};

static uint16_t u16(const uint8_t *d, size_t i) { return uint16_t(d[i] << 8 | d[i + 1]); }
static int16_t s16(const uint8_t *d, size_t i) { return int16_t(u16(d, i)); }

void buildRequest(uint8_t cmd, uint8_t out[7]) {
    // DD A5 <cmd> 00 <checksum hi> <checksum lo> 77
    uint16_t checksum = uint16_t(0x10000 - cmd);
    const uint8_t frame[7] = {0xDD, 0xA5, cmd, 0x00, uint8_t(checksum >> 8), uint8_t(checksum), 0x77};
    memcpy(out, frame, 7);
}

bool parseBasicInfo(const uint8_t *d, size_t len, BasicInfo &out) {
    if (len < 23)
        return false;
    uint8_t ntc = d[22];
    if (len < 23 + 2u * ntc)
        return false;

    out = {};
    out.voltage = u16(d, 0) / 100.0f;
    out.current = s16(d, 2) / 100.0f;
    out.remainingAh = u16(d, 4) / 100.0f;
    out.nominalAh = u16(d, 6) / 100.0f;
    out.cycles = u16(d, 8);
    uint16_t date = u16(d, 10);
    out.year = 2000 + (date >> 9);
    out.month = (date >> 5) & 0x0F;
    out.day = date & 0x1F;
    out.balancing = u16(d, 12) | uint32_t(u16(d, 14)) << 16;
    out.protection = u16(d, 16);
    out.soc = d[19];
    out.chargeFet = d[20] & 0x01;
    out.dischargeFet = d[20] & 0x02;
    out.cellCount = d[21];
    out.tempCount = ntc < MAX_TEMPS ? ntc : MAX_TEMPS;
    for (int i = 0; i < out.tempCount; i++)
        out.temps[i] = (u16(d, 23 + 2 * i) - 2731) / 10.0f;

    // Newer firmware appends: humidity(1), alarm(2), full charge capacity(2), remaining(2), balance current(2)
    size_t ext = 23 + 2 * ntc;
    if (len >= ext + 5)
        out.fullChargeAh = u16(d, ext + 3) / 100.0f;
    return true;
}

int parseCells(const uint8_t *d, size_t len, float *out, int maxCells) {
    int n = 0;
    for (size_t i = 0; i + 1 < len && n < maxCells; i += 2)
        out[n++] = u16(d, i) / 1000.0f;
    return n;
}

bool FrameAssembler::feed(const uint8_t *data, size_t len) {
    if (len_ + len > sizeof(buf_))
        len_ = 0; // garbage piled up; start over
    if (len > sizeof(buf_))
        return false;
    memcpy(buf_ + len_, data, len);
    len_ += len;
    return extract();
}

bool FrameAssembler::extract() {
    while (len_ > 0) {
        // Drop bytes until a frame start.
        size_t start = 0;
        while (start < len_ && buf_[start] != 0xDD)
            start++;
        memmove(buf_, buf_ + start, len_ - start);
        len_ -= start;
        if (len_ < 4)
            return false;

        size_t total = 4 + buf_[3] + 3; // header + payload + checksum + 0x77
        if (len_ < total)
            return false;

        uint8_t cmd = buf_[1], status = buf_[2], payloadLen = buf_[3];
        uint16_t sum = 0;
        for (size_t i = 2; i < total - 3; i++)
            sum += buf_[i];
        bool valid = buf_[total - 1] == 0x77 && uint16_t(0x10000 - sum) == u16(buf_, total - 3) && status == 0 &&
                     payloadLen <= MAX_PAYLOAD;
        if (valid) {
            frameCmd_ = cmd;
            frameLen_ = payloadLen;
            memcpy(frame_, buf_ + 4, payloadLen);
        }
        memmove(buf_, buf_ + total, len_ - total);
        len_ -= total;
        if (valid)
            return true;
    }
    return false;
}

} // namespace jbd
