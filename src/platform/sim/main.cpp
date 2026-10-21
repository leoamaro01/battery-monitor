// Desktop simulator: the real UI in a 480x480 SDL window, fed with fake battery and weather data.
//
//   pio run -e simulator -t exec                              (cycles through scenarios every 20 s)
//   .pio/build/simulator/program --scenario 2                 (stick to one scenario)
//   .pio/build/simulator/program --snapshot out/ --scenario 1 (render each page to out/pageN.bmp, no window)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lvgl.h>
#include <string>
#include <thread>

#include "../../core/jbd.h"
#include "../../core/platform.h"
#include "../../core/state.h"
#include "../../core/weather.h"
#include "../../ui/ui.h"

// ---------------------------------------------------------------- platform hooks

static const auto g_start = std::chrono::steady_clock::now();

uint32_t platform_millis() {
    return uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
}

void platform_set_backlight(float level) { printf("[sim] backlight %.0f%%\n", level * 100); }

// ---------------------------------------------------------------- fake data

// Real replies captured from the two batteries with nRF Connect.
static const char *CAPTURED[BATTERY_COUNT][2] = {
    {"DD0300220536FCC75C036D600097317E00000000000066560304010BF70000006B305C030000F8AE77",
     "DD0400080CFF0D000D000D00FEC677"},
    {"DD0300220533FCAD57D56D60009C322300000000000066520304010BFC0000006B3057D50000F78577",
     "DD0400080CFF0CFF0D000D01FDC777"},
};

static const char *SAMPLE_WEATHER = R"({"current":{"temperature_2m":18.4,"relative_humidity_2m":64,
    "weather_code":2,"wind_speed_10m":11.2,"is_day":1},"daily":{"temperature_2m_max":[21.3],"temperature_2m_min":[12.1]}})";

static const char *SCENARIOS[] = {"discharging", "charging", "battery B lost + imbalance", "low battery"};
constexpr int SCENARIO_COUNT = sizeof SCENARIOS / sizeof *SCENARIOS;

static size_t hexToBytes(const char *hex, uint8_t *out, size_t max) {
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1] && n < max; p += 2) {
        char byte[3] = {p[0], p[1], 0};
        out[n++] = uint8_t(strtoul(byte, nullptr, 16));
    }
    return n;
}

static void loadCaptured(BatteryState &b, int index) {
    jbd::FrameAssembler fa;
    uint8_t frame[64];
    for (const char *hex : CAPTURED[index]) {
        size_t n = hexToBytes(hex, frame, sizeof frame);
        if (!fa.feed(frame, n)) {
            fprintf(stderr, "captured frame %d did not parse\n", index);
            exit(1);
        }
        if (fa.cmd() == jbd::CMD_BASIC_INFO)
            jbd::parseBasicInfo(fa.payload(), fa.payloadLen(), b.info);
        else
            b.cellCount = jbd::parseCells(fa.payload(), fa.payloadLen(), b.cells, jbd::MAX_CELLS);
    }
}

static void updateFakeData(int scenario, uint32_t now) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    AppState &s = g_state;
    s.wifiConnected = true;
    s.timeValid = true;
    if (!s.weather.valid)
        weather::parse(SAMPLE_WEATHER, strlen(SAMPLE_WEATHER), s.weather);

    float wobble = sinf(now / 3000.0f); // gentle movement so you can see the UI update
    for (int i = 0; i < BATTERY_COUNT; i++) {
        BatteryState &b = s.batteries[i];
        b.label = 'A' + i;
        snprintf(b.address, sizeof b.address, "sim:%c", b.label);
        loadCaptured(b, i % 2);
        b.link = LinkState::Connected;
        b.valid = true;
        b.updatedMs = now;
        b.info.current += 0.3f * wobble;

        switch (scenario) {
        case 1: // charging from solar
            b.info.current = 24.0f + wobble;
            b.info.voltage += 0.45f;
            for (int c = 0; c < b.cellCount; c++)
                b.cells[c] += 0.11f + 0.002f * c;
            b.info.balancing = i == 0 ? 0b0100 : 0;
            break;
        case 2: // battery B dropped out a while ago and has drifted
            if (i == 1) {
                b.link = LinkState::Lost;
                b.updatedMs = now > 45000 ? now - 45000 : 0;
                b.info.soc -= 9;
                for (int c = 0; c < b.cellCount; c++)
                    b.cells[c] -= 0.035f;
                b.cells[2] -= 0.04f;
            }
            break;
        case 3: // nearly empty
            b.info.soc = 12 - i;
            b.info.remainingAh = b.info.capacityAh() * b.info.soc / 100;
            b.info.voltage -= 0.6f;
            for (int c = 0; c < b.cellCount; c++)
                b.cells[c] -= 0.15f + 0.01f * c;
            break;
        }
    }
}

// ---------------------------------------------------------------- snapshot mode

static bool writeBmp(const std::string &path, const lv_draw_buf_t *buf) {
    const uint32_t w = buf->header.w, h = buf->header.h, rowBytes = (w * 3 + 3) & ~3u;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    uint32_t fileSize = 54 + rowBytes * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t pad[3] = {};
    for (int y = h - 1; y >= 0; y--) { // BMP rows go bottom-up; LVGL RGB888 is already B,G,R
        fwrite(buf->data + y * buf->header.stride, 1, w * 3, f);
        fwrite(pad, 1, rowBytes - w * 3, f);
    }
    fclose(f);
    return true;
}

static int runSnapshots(const std::string &dir, int scenario) {
    static uint8_t fb[480 * 48 * 2];
    lv_display_t *disp = lv_display_create(480, 480);
    lv_display_set_buffers(disp, fb, nullptr, sizeof fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });

    updateFakeData(scenario, platform_millis());
    ui_init();
    for (int page = 0; page < 3; page++) {
        ui_show_page(page);
        lv_obj_update_layout(lv_screen_active());
        lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
        std::string path = dir + "/page" + std::to_string(page + 1) + ".bmp";
        if (!snap || !writeBmp(path, snap)) {
            fprintf(stderr, "failed to write %s\n", path.c_str());
            return 1;
        }
        lv_draw_buf_destroy(snap);
        printf("wrote %s\n", path.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
    int scenario = -1; // -1 = cycle
    const char *snapshotDir = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc)
            scenario = atoi(argv[++i]) % SCENARIO_COUNT;
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)
            snapshotDir = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--scenario 0-%d] [--snapshot DIR]\n", argv[0], SCENARIO_COUNT - 1);
            for (int s = 0; s < SCENARIO_COUNT; s++)
                fprintf(stderr, "  scenario %d: %s\n", s, SCENARIOS[s]);
            return 2;
        }
    }

    lv_init();
    lv_tick_set_cb(platform_millis);
    if (snapshotDir)
        return runSnapshots(snapshotDir, scenario < 0 ? 0 : scenario);

    lv_sdl_window_create(480, 480);
    lv_sdl_mouse_create();
    updateFakeData(scenario < 0 ? 0 : scenario, platform_millis());
    ui_init();

    int shown = -2;
    uint32_t lastData = 0;
    while (true) {
        uint32_t now = platform_millis();
        if (now - lastData >= 1000) {
            int active = scenario >= 0 ? scenario : int(now / 20000) % SCENARIO_COUNT;
            if (active != shown) {
                printf("[sim] scenario %d: %s\n", active, SCENARIOS[active]);
                shown = active;
            }
            updateFakeData(active, now);
            lastData = now;
        }
        uint32_t idle = lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(idle < 10 ? idle : 10));
    }
}
