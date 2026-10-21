// User settings. Wi-Fi credentials live in secrets.h (copy secrets.example.h).
#pragma once

// ---- Batteries ----
#define BATTERY_COUNT 2
// Batteries are matched by MAC on the ESP32 and labelled A, B, ... in this order.
// Leave a slot empty ("") to accept any device whose name starts with BATTERY_NAME_PREFIX.
#define BATTERY_ADDRESSES {"a5:c2:37:40:f9:ae", "a5:c2:37:46:2e:4b"}
#define BATTERY_NAME_PREFIX "DP04S"
#define BATTERY_POLL_MS 5000

// ---- Pack warnings ----
#define SOC_DRIFT_WARN 5             // % difference between batteries
#define AVG_CELL_DRIFT_WARN 0.020f   // V between the batteries' average cell voltage
#define CELL_SPREAD_WARN 0.050f      // V between the lowest and highest cell in the pack
#define CURRENT_MISMATCH_WARN 1.0f   // A; in series both should read the same
#define STALE_AFTER_MS 30000         // a battery reading older than this is flagged

// ---- Weather (Open-Meteo, no API key needed) ----
#define WEATHER_LATITUDE 40.4168f
#define WEATHER_LONGITUDE -3.7038f
#define WEATHER_REFRESH_MS (15 * 60 * 1000)

// ---- Clock ----
// POSIX TZ string, see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER "pool.ntp.org"
#define CLOCK_24H 1

// ---- Screen ----
#define SCREEN_SLEEP_MS (2 * 60 * 1000) // touch to wake; 0 = never sleep
#define SCREEN_BRIGHTNESS 0.8f          // 0..1
