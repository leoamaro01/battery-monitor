# Battery monitor

A 4" touchscreen gadget (Guition ESP32-4848S040C) that shows the time, the weather and the state of two
Eco-Worthy 12V 280Ah LiFePO4 batteries (JBD BMS) wired in series, read over Bluetooth.

```
src/core/          protocol parsing, pack math, weather parsing (shared, no platform code)
src/ui/            LVGL pages: home (clock + weather), pack, per-battery cells
src/platform/sim/  desktop simulator with fake data
src/platform/esp32 firmware: display, Wi-Fi, NTP, Open-Meteo, BLE polling
include/config.h   batteries, warning thresholds, location, timezone, screen timeout
tools/             jbd_monitor.py, the Python prototype (uv run tools/jbd_monitor.py)
```

## Setup (macOS)

```sh
brew install platformio sdl2
git submodule update --init      # board definitions in boards/
cp include/secrets.example.h include/secrets.h   # then add your Wi-Fi details
```

Set your location (`WEATHER_LATITUDE`/`WEATHER_LONGITUDE`) and `TIMEZONE` in `include/config.h`.

## Simulator

```sh
pio run -e simulator -t exec                       # window; cycles through fake scenarios every 20 s
.pio/build/simulator/program --scenario 2          # stick to one scenario (0-3)
.pio/build/simulator/program --snapshot shots/     # render each page to shots/pageN.bmp, no window
```

### Real data in the simulator

```sh
.pio/build/simulator/program --live              # real weather + real batteries
.pio/build/simulator/program --live-weather      # real weather, fake batteries
.pio/build/simulator/program --live-batteries    # real batteries, fake weather
```

Run these from the project root. Weather comes from Open-Meteo via `curl`. Batteries are read by
`uv run tools/jbd_monitor.py --json`, which handles Bluetooth on the desktop and hands the raw BMS
replies to the simulator, where the same C++ parser as the firmware decodes them. Close the
Eco-Worthy app first; the batteries take only one connection. `--bridge CMD` swaps in another
command that prints the same JSON lines.

Drag with the mouse to swipe between pages. The screen goes black after `SCREEN_SLEEP_MS` without
input; click to wake.

## Flashing the board

```sh
pio run -e esp32-4848S040CIY1 -t upload -t monitor
```

Use a USB-C cable to a desktop port or a phone charger. If no `/dev/cu.usbserial-*` /
`/dev/cu.wchusbserial*` port appears, install the CH340 driver from WCH.

The board keeps a BLE connection to both batteries, so the Eco-Worthy phone app can't connect while it
runs.

## Notes

- After changing `include/lv_conf.h`, delete `.pio/` so LVGL rebuilds with it.
- `src/ui/fonts/` holds the big clock and number fonts, generated from LVGL's Montserrat with
  `npx lv_font_conv` (command in each file's header). Regenerate to add characters or sizes.
