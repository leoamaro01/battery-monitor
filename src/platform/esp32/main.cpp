// Firmware for the Guition ESP32-4848S040C: display + touch, Wi-Fi clock and weather, BLE batteries.
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp32_smartdisplay.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill in your Wi-Fi details"
#endif

#include "../../core/platform.h"
#include "../../core/state.h"
#include "../../core/weather.h"
#include "../../ui/ui.h"
#include "ble_batteries.h"

uint32_t platform_millis() { return millis(); }

void platform_set_backlight(float level) { smartdisplay_lcd_set_backlight(level); }

namespace {

constexpr uint32_t WEATHER_RETRY_MS = 60 * 1000;

bool fetchWeather() {
    char url[320];
    weather::buildUrl(url, sizeof url);
    WiFiClientSecure tls;
    tls.setInsecure(); // public, read-only data; skip certificate checks
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(tls, url))
        return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[weather] HTTP %d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    WeatherState w;
    if (!weather::parse(body.c_str(), body.length(), w)) {
        Serial.println("[weather] could not parse response");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.weather = w;
    return true;
}

// Wi-Fi status, clock sync and weather, off the UI thread so slow HTTP never freezes the screen.
void networkTask(void *) {
    uint32_t nextWeather = 0;
    for (;;) {
        bool connected = WiFi.isConnected();
        bool timeValid = time(nullptr) > 1700000000; // set once NTP has answered
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state.wifiConnected = connected;
            g_state.timeValid = timeValid;
        }
        if (connected && int32_t(millis() - nextWeather) >= 0)
            nextWeather = millis() + (fetchWeather() ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS);
        delay(1000);
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    smartdisplay_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    ui_init();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTzTime(TIMEZONE, NTP_SERVER);

    xTaskCreatePinnedToCore(networkTask, "net", 8192, nullptr, 1, nullptr, 0);
    ble_batteries_start();
}

void loop() {
    uint32_t idle = lv_timer_handler();
    delay(idle < 5 ? idle : 5);
}
