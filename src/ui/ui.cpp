#include "ui.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <lvgl.h>

#include "../core/platform.h"
#include "../core/state.h"
#include "../core/weather.h"

LV_FONT_DECLARE(font_clock_128);
LV_FONT_DECLARE(font_big_72);

namespace {

// ---- Palette ----
const lv_color_t BG = lv_color_hex(0x0B0F14);
const lv_color_t CARD = lv_color_hex(0x161C24);
const lv_color_t TRACK = lv_color_hex(0x262E38);
const lv_color_t TEXT = lv_color_hex(0xE6EDF3);
const lv_color_t MUTED = lv_color_hex(0x8B949E);
const lv_color_t GREEN = lv_color_hex(0x3FB950);
const lv_color_t AMBER = lv_color_hex(0xD29922);
const lv_color_t RED = lv_color_hex(0xF85149);
const lv_color_t BLUE = lv_color_hex(0x58A6FF);

constexpr int PAGE_COUNT = 3;
constexpr uint32_t REFRESH_MS = 500;
// Cell bars span the useful LiFePO4 range.
constexpr int CELL_BAR_MIN_MV = 3000, CELL_BAR_MAX_MV = 3500;

struct BatteryWidgets {
    lv_obj_t *title, *link, *soc, *socBar, *stats, *cellBars[jbd::MAX_CELLS], *cellLabels[jbd::MAX_CELLS], *footer;
};

struct {
    lv_obj_t *tileview, *dots[PAGE_COUNT];
    // Home
    lv_obj_t *statusPack, *statusWifi, *statusBle, *clock, *date, *weatherTemp, *weatherDesc, *weatherDetail;
    // Pack
    lv_obj_t *packArc, *packSoc, *packAh, *packState, *packStats[6], *packWarnings;
    // Batteries
    BatteryWidgets batt[BATTERY_COUNT];
    // Sleep
    lv_obj_t *sleepOverlay;
    bool sleeping;
} ui;

void setText(lv_obj_t *label, const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    lv_label_set_text(label, buf);
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text = "") {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *card(lv_obj_t *parent, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_clickable(c, false); // let swipes reach the tileview
    return c;
}

lv_obj_t *bar(lv_obj_t *parent, int w, int h, int min, int max) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, min, max);
    lv_obj_set_style_bg_color(b, TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, GREEN, LV_PART_INDICATOR);
    lv_obj_set_clickable(b, false);
    return b;
}

lv_color_t socColor(int soc) { return soc <= 15 ? RED : soc <= 30 ? AMBER : GREEN; }

const char *linkText(LinkState s) {
    switch (s) {
    case LinkState::Searching: return "searching";
    case LinkState::Connecting: return "connecting";
    case LinkState::Connected: return "connected";
    case LinkState::Lost: return "lost";
    }
    return "";
}

// ---------------------------------------------------------------- pages

void buildHome(lv_obj_t *page) {
    ui.statusPack = label(page, &lv_font_montserrat_20, MUTED, "Pack --");
    lv_obj_align(ui.statusPack, LV_ALIGN_TOP_LEFT, 24, 20);
    ui.statusBle = label(page, &lv_font_montserrat_20, MUTED, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(ui.statusBle, LV_ALIGN_TOP_RIGHT, -24, 20);
    ui.statusWifi = label(page, &lv_font_montserrat_20, MUTED, LV_SYMBOL_WIFI);
    lv_obj_align(ui.statusWifi, LV_ALIGN_TOP_RIGHT, -56, 20);

    ui.clock = label(page, &font_clock_128, TEXT, "--:--");
    lv_obj_align(ui.clock, LV_ALIGN_TOP_MID, 0, 70);
    ui.date = label(page, &lv_font_montserrat_24, MUTED, "");
    lv_obj_align(ui.date, LV_ALIGN_TOP_MID, 0, 212);

    lv_obj_t *w = card(page, 432, 150);
    lv_obj_align(w, LV_ALIGN_BOTTOM_MID, 0, -44);
    ui.weatherTemp = label(w, &font_big_72, TEXT, "--°");
    lv_obj_align(ui.weatherTemp, LV_ALIGN_LEFT_MID, 4, 0);
    ui.weatherDesc = label(w, &lv_font_montserrat_32, TEXT, "No weather yet");
    lv_obj_align(ui.weatherDesc, LV_ALIGN_TOP_LEFT, 150, 14);
    ui.weatherDetail = label(w, &lv_font_montserrat_20, MUTED, "");
    lv_obj_align(ui.weatherDetail, LV_ALIGN_BOTTOM_LEFT, 150, -14);
}

void buildPack(lv_obj_t *page) {
    lv_obj_t *title = label(page, &lv_font_montserrat_24, MUTED, "Battery pack");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 20);

    ui.packArc = lv_arc_create(page);
    lv_obj_set_size(ui.packArc, 220, 220);
    lv_arc_set_rotation(ui.packArc, 135);
    lv_arc_set_bg_angles(ui.packArc, 0, 270);
    lv_arc_set_range(ui.packArc, 0, 100);
    lv_obj_remove_style(ui.packArc, nullptr, LV_PART_KNOB);
    lv_obj_set_clickable(ui.packArc, false);
    lv_obj_set_style_arc_width(ui.packArc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.packArc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.packArc, TRACK, LV_PART_MAIN);
    lv_obj_align(ui.packArc, LV_ALIGN_TOP_LEFT, 16, 60);

    ui.packSoc = label(page, &font_big_72, TEXT, "--");
    lv_obj_align_to(ui.packSoc, ui.packArc, LV_ALIGN_CENTER, 0, -8);
    ui.packAh = label(page, &lv_font_montserrat_20, MUTED, "");
    lv_obj_align_to(ui.packAh, ui.packArc, LV_ALIGN_CENTER, 0, 44);

    ui.packState = label(page, &lv_font_montserrat_24, TEXT, "Waiting for batteries");
    lv_obj_set_width(ui.packState, 200);
    lv_obj_align(ui.packState, LV_ALIGN_TOP_LEFT, 256, 80);

    // 2x3 grid of stats under the arc.
    for (int i = 0; i < 6; i++) {
        lv_obj_t *c = card(page, 136, 64);
        lv_obj_set_style_pad_all(c, 10, 0);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 16 + (i % 3) * 152, 292 + (i / 3) * 76);
        ui.packStats[i] = label(c, &lv_font_montserrat_16, TEXT, "");
        lv_obj_center(ui.packStats[i]);
        lv_obj_set_style_text_align(ui.packStats[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    ui.packWarnings = label(page, &lv_font_montserrat_16, AMBER, "");
    lv_obj_set_width(ui.packWarnings, 200);
    lv_obj_align(ui.packWarnings, LV_ALIGN_TOP_LEFT, 256, 150);
}

void buildBatteries(lv_obj_t *page) {
    const int w = (480 - 16 * (BATTERY_COUNT + 1)) / BATTERY_COUNT;
    for (int b = 0; b < BATTERY_COUNT; b++) {
        BatteryWidgets &bw = ui.batt[b];
        lv_obj_t *c = card(page, w, 420);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 16 + b * (w + 16), 16);

        bw.title = label(c, &lv_font_montserrat_24, TEXT, "Battery ?");
        lv_obj_align(bw.title, LV_ALIGN_TOP_LEFT, 0, 0);
        bw.link = label(c, &lv_font_montserrat_16, MUTED, "");
        lv_obj_align(bw.link, LV_ALIGN_TOP_LEFT, 0, 32);

        bw.soc = label(c, &lv_font_montserrat_48, TEXT, "--");
        lv_obj_align(bw.soc, LV_ALIGN_TOP_LEFT, 0, 58);
        bw.socBar = bar(c, w - 32, 10, 0, 100);
        lv_obj_align(bw.socBar, LV_ALIGN_TOP_LEFT, 0, 116);

        bw.stats = label(c, &lv_font_montserrat_16, TEXT, "");
        lv_obj_set_style_text_line_space(bw.stats, 4, 0);
        lv_obj_align(bw.stats, LV_ALIGN_TOP_LEFT, 0, 136);

        for (int i = 0; i < jbd::MAX_CELLS; i++) {
            bw.cellLabels[i] = label(c, &lv_font_montserrat_16, MUTED, "");
            lv_obj_align(bw.cellLabels[i], LV_ALIGN_TOP_LEFT, 0, 236 + i * 28);
            bw.cellBars[i] = bar(c, w - 32 - 96, 12, CELL_BAR_MIN_MV, CELL_BAR_MAX_MV);
            lv_obj_align(bw.cellBars[i], LV_ALIGN_TOP_LEFT, 96, 240 + i * 28);
            lv_obj_set_hidden(bw.cellLabels[i], true);
            lv_obj_set_hidden(bw.cellBars[i], true);
        }

        bw.footer = label(c, &lv_font_montserrat_16, MUTED, "");
        lv_obj_align(bw.footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

// ---------------------------------------------------------------- refresh

void refreshHome(const AppState &s, const PackSummary &p) {
    if (s.timeValid) {
        time_t now = time(nullptr);
        tm t;
        localtime_r(&now, &t);
        char buf[48];
        strftime(buf, sizeof buf, CLOCK_24H ? "%H:%M" : "%I:%M", &t);
        lv_label_set_text(ui.clock, buf);
        strftime(buf, sizeof buf, "%A, %e %B", &t);
        lv_label_set_text(ui.date, buf);
    }

    if (p.valid) {
        setText(ui.statusPack, "Pack %d%%  %.1f V", p.soc, p.voltage);
        lv_obj_set_style_text_color(ui.statusPack, p.warnings ? AMBER : socColor(p.soc), 0);
    }
    lv_obj_set_style_text_color(ui.statusWifi, s.wifiConnected ? BLUE : MUTED, 0);
    bool allLinked = true;
    for (const auto &b : s.batteries)
        allLinked &= b.link == LinkState::Connected;
    lv_obj_set_style_text_color(ui.statusBle, allLinked ? BLUE : MUTED, 0);

    const WeatherState &w = s.weather;
    if (w.valid) {
        setText(ui.weatherTemp, "%.0f°", w.tempC);
        lv_label_set_text(ui.weatherDesc, weather::describe(w.code));
        setText(ui.weatherDetail, "H %.0f°  L %.0f°  " LV_SYMBOL_TINT " %d%%  %.0f km/h", w.highC, w.lowC, w.humidity, w.windKmh);
    }
}

void refreshPack(const PackSummary &p) {
    if (!p.valid)
        return;
    lv_arc_set_value(ui.packArc, p.soc);
    lv_obj_set_style_arc_color(ui.packArc, socColor(p.soc), LV_PART_INDICATOR);
    setText(ui.packSoc, "%d%%", p.soc);
    lv_obj_align_to(ui.packSoc, ui.packArc, LV_ALIGN_CENTER, 0, -8);
    setText(ui.packAh, "%.1f / %.0f Ah", p.remainingAh, p.capacityAh);
    lv_obj_align_to(ui.packAh, ui.packArc, LV_ALIGN_CENTER, 0, 44);

    if (p.current < -0.1f) {
        setText(ui.packState, "Discharging\n%.0f W", -p.power);
        lv_obj_set_style_text_color(ui.packState, TEXT, 0);
    } else if (p.current > 0.1f) {
        setText(ui.packState, "Charging\n%.0f W", p.power);
        lv_obj_set_style_text_color(ui.packState, GREEN, 0);
    } else {
        lv_label_set_text(ui.packState, "Idle");
        lv_obj_set_style_text_color(ui.packState, MUTED, 0);
    }

    char runtime[32];
    if (p.hoursLeft >= 0)
        snprintf(runtime, sizeof runtime, "%.1f h", p.hoursLeft);
    else
        snprintf(runtime, sizeof runtime, "--");
    setText(ui.packStats[0], "Voltage\n%.2f V", p.voltage);
    setText(ui.packStats[1], "Current\n%+.1f A", p.current);
    setText(ui.packStats[2], "Time left\n%s", runtime);
    setText(ui.packStats[3], "Energy\n%.2f kWh", p.energyKwh);
    setText(ui.packStats[4], "Cell spread\n%.0f mV", p.cellSpread * 1000);
    setText(ui.packStats[5], "Max temp\n%.1f °C", p.tempMax);

    char warn[256] = "";
    size_t n = 0;
    auto add = [&](const char *fmt, auto... args) {
        if (n < sizeof warn)
            n += snprintf(warn + n, sizeof warn - n, fmt, args...);
    };
    if (p.warnings & WARN_PROTECTION)
        add(LV_SYMBOL_WARNING " BMS protection active\n");
    if (p.warnings & WARN_STALE)
        add(LV_SYMBOL_WARNING " Readings out of date\n");
    if (p.warnings & WARN_BATTERY_IMBALANCE)
        add(LV_SYMBOL_WARNING " Batteries %.0f mV apart\n", p.avgCellDrift * 1000);
    if (p.warnings & WARN_SOC_DRIFT)
        add(LV_SYMBOL_WARNING " SoC differs %d%%\n", p.socDrift);
    if (p.warnings & WARN_CELL_SPREAD)
        add(LV_SYMBOL_WARNING " Cell spread high\n");
    if (p.warnings & WARN_CURRENT_MISMATCH)
        add(LV_SYMBOL_WARNING " Currents disagree\n");
    if (!p.warnings)
        add(LV_SYMBOL_OK " All good");
    lv_label_set_text(ui.packWarnings, warn);
    bool severe = p.warnings & (WARN_PROTECTION | WARN_BATTERY_IMBALANCE | WARN_CELL_SPREAD);
    lv_obj_set_style_text_color(ui.packWarnings, !p.warnings ? GREEN : severe ? RED : AMBER, 0);
}

void refreshBatteries(const AppState &s, uint32_t nowMs) {
    for (int b = 0; b < BATTERY_COUNT; b++) {
        const BatteryState &bs = s.batteries[b];
        BatteryWidgets &bw = ui.batt[b];
        setText(bw.title, "Battery %c", bs.label);
        bool stale = bs.valid && nowMs - bs.updatedMs > STALE_AFTER_MS;
        if (stale)
            setText(bw.link, "%s, %lus old", linkText(bs.link), (unsigned long)((nowMs - bs.updatedMs) / 1000));
        else
            lv_label_set_text(bw.link, linkText(bs.link));
        lv_obj_set_style_text_color(bw.link, bs.link == LinkState::Connected && !stale ? GREEN : AMBER, 0);
        if (!bs.valid)
            continue;

        const jbd::BasicInfo &i = bs.info;
        setText(bw.soc, "%d%%", i.soc);
        lv_bar_set_value(bw.socBar, i.soc, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bw.socBar, socColor(i.soc), LV_PART_INDICATOR);
        setText(bw.stats, "%.2f V   %+.2f A\n%.1f / %.0f Ah\n%.1f °C   %u cycles", i.voltage, i.current, i.remainingAh,
                i.capacityAh(), i.tempCount ? i.temps[0] : 0.0f, i.cycles);

        float lo = 99, hi = 0;
        for (int c = 0; c < bs.cellCount; c++) {
            lo = bs.cells[c] < lo ? bs.cells[c] : lo;
            hi = bs.cells[c] > hi ? bs.cells[c] : hi;
        }
        for (int c = 0; c < jbd::MAX_CELLS; c++) {
            bool shown = c < bs.cellCount;
            lv_obj_set_hidden(bw.cellLabels[c], !shown);
            lv_obj_set_hidden(bw.cellBars[c], !shown);
            if (!shown)
                continue;
            int mv = int(bs.cells[c] * 1000 + 0.5f);
            setText(bw.cellLabels[c], "%d  %.3f", c + 1, bs.cells[c]);
            lv_bar_set_value(bw.cellBars[c], mv, LV_ANIM_OFF);
            // Highlight the extremes once the spread is worth noticing.
            bool extreme = hi - lo >= 0.010f && (bs.cells[c] == lo || bs.cells[c] == hi);
            bool balancing = i.balancing & (1u << c);
            lv_obj_set_style_bg_color(bw.cellBars[c], balancing ? BLUE : extreme ? AMBER : GREEN, LV_PART_INDICATOR);
        }

        const char *fets = i.chargeFet && i.dischargeFet ? "" : !i.chargeFet ? "  CHG OFF" : "  DSG OFF";
        setText(bw.footer, "Made %04u-%02u-%02u%s", i.year, i.month, i.day, fets);
        lv_obj_set_style_text_color(bw.footer, *fets ? RED : MUTED, 0);
    }
}

// ---------------------------------------------------------------- sleep

void wake() {
    if (!ui.sleeping)
        return;
    ui.sleeping = false;
    lv_obj_set_hidden(ui.sleepOverlay, true);
    platform_set_backlight(SCREEN_BRIGHTNESS);
    lv_display_trigger_activity(nullptr);
}

void updateSleep() {
    if (SCREEN_SLEEP_MS == 0 || ui.sleeping || lv_display_get_inactive_time(nullptr) < SCREEN_SLEEP_MS)
        return;
    ui.sleeping = true;
    lv_obj_set_hidden(ui.sleepOverlay, false);
    platform_set_backlight(0);
}

void buildSleepOverlay() {
    // A black, touch-swallowing layer: the wake-up tap shouldn't also press whatever is underneath.
    ui.sleepOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ui.sleepOverlay);
    lv_obj_set_size(ui.sleepOverlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.sleepOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.sleepOverlay, LV_OPA_COVER, 0);
    lv_obj_set_clickable(ui.sleepOverlay, true);
    lv_obj_set_hidden(ui.sleepOverlay, true);
    lv_obj_add_event_cb(ui.sleepOverlay, [](lv_event_t *) { wake(); }, LV_EVENT_PRESSED, nullptr);
}

// ---------------------------------------------------------------- glue

void refresh(lv_timer_t *) {
    AppState s;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        s = g_state;
    }
    uint32_t now = platform_millis();
    PackSummary p = summarizePack(s, now);
    refreshHome(s, p);
    refreshPack(p);
    refreshBatteries(s, now);
    updateSleep();
}

void updateDots() {
    int32_t active = lv_obj_get_index(lv_tileview_get_tile_active(ui.tileview));
    for (int i = 0; i < PAGE_COUNT; i++)
        lv_obj_set_style_bg_color(ui.dots[i], i == active ? TEXT : TRACK, 0);
}

} // namespace

void ui_show_page(int index) {
    lv_tileview_set_tile_by_index(ui.tileview, index, 0, LV_ANIM_OFF);
    updateDots();
}

void ui_init() {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, BG, 0);
    lv_obj_set_style_text_color(screen, TEXT, 0);

    lv_obj_t *tv = ui.tileview = lv_tileview_create(screen);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *pages[PAGE_COUNT];
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_dir_t dir = lv_dir_t((i > 0 ? LV_DIR_LEFT : 0) | (i < PAGE_COUNT - 1 ? LV_DIR_RIGHT : 0));
        pages[i] = lv_tileview_add_tile(tv, i, 0, dir);
    }
    buildHome(pages[0]);
    buildPack(pages[1]);
    buildBatteries(pages[2]);

    // Page indicator dots, above the pages so they stay put while swiping.
    for (int i = 0; i < PAGE_COUNT; i++) {
        ui.dots[i] = lv_obj_create(screen);
        lv_obj_remove_style_all(ui.dots[i]);
        lv_obj_set_size(ui.dots[i], 8, 8);
        lv_obj_set_style_radius(ui.dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ui.dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ui.dots[i], i == 0 ? TEXT : TRACK, 0);
        lv_obj_align(ui.dots[i], LV_ALIGN_BOTTOM_MID, (i - (PAGE_COUNT - 1) / 2.0f) * 20, -14);
    }
    lv_obj_add_event_cb(tv, [](lv_event_t *) { updateDots(); }, LV_EVENT_VALUE_CHANGED, nullptr);

    buildSleepOverlay();
    platform_set_backlight(SCREEN_BRIGHTNESS);
    lv_timer_create(refresh, REFRESH_MS, nullptr);
    refresh(nullptr);
}
