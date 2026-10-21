// Background task that finds the batteries, keeps a BLE connection to each and polls them
// into g_state. Same flow as tools/jbd_monitor.py.
#pragma once

void ble_batteries_start();
