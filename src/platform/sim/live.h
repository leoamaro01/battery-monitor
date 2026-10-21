// Real data for the simulator: weather via curl, batteries via tools/jbd_monitor.py --json
// (which handles Bluetooth on the desktop). Both run on background threads and write g_state.
#pragma once

void live_weather_start();
void live_batteries_start(const char *bridgeCommand);
