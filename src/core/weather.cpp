#include "weather.h"

#include <ArduinoJson.h>
#include <cstdio>

namespace weather {

void buildUrl(char *out, size_t size) {
    snprintf(out, size,
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
             "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=1",
             WEATHER_LATITUDE, WEATHER_LONGITUDE);
}

bool parse(const char *json, size_t len, WeatherState &out) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len))
        return false;
    JsonObject cur = doc["current"];
    JsonObject daily = doc["daily"];
    if (cur.isNull() || daily.isNull())
        return false;
    out.tempC = cur["temperature_2m"];
    out.humidity = cur["relative_humidity_2m"];
    out.code = cur["weather_code"];
    out.windKmh = cur["wind_speed_10m"];
    out.isDay = cur["is_day"] | 1;
    out.highC = daily["temperature_2m_max"][0];
    out.lowC = daily["temperature_2m_min"][0];
    out.valid = true;
    return true;
}

const char *describe(int code) {
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "Unknown";
    }
}

} // namespace weather
