// Small hooks each platform (simulator, ESP32) implements for the shared code.
#pragma once

#include <cstdint>

uint32_t platform_millis();
void platform_set_backlight(float level); // 0 = off, 1 = full
