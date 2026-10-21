// Open-Meteo forecast request and response parsing.
#pragma once

#include <cstddef>

#include "state.h"

namespace weather {

// Writes the request URL for the configured location into `out`.
void buildUrl(char *out, size_t size);
bool parse(const char *json, size_t len, WeatherState &out);
// Short description for a WMO weather code ("Clear", "Rain", ...).
const char *describe(int code);

} // namespace weather
