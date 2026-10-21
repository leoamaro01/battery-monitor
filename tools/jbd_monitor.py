# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak>=0.22"]
# ///
"""Poll JBD-BMS LiFePO4 batteries over BLE and show them as one series pack.

Run with:  uv run jbd_monitor.py            (scan, connect, poll every 5 s)
           uv run jbd_monitor.py --scan     (just list matching batteries)
           uv run jbd_monitor.py --once     (single reading, then exit)
           uv run jbd_monitor.py --json     (one JSON line per battery per poll, for the simulator)

Only read commands (0x03, 0x04) are ever sent. Nothing is written to the BMS.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from dataclasses import dataclass, field

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

NOTIFY_CHAR = "0000ff01-0000-1000-8000-00805f9b34fb"
WRITE_CHAR = "0000ff02-0000-1000-8000-00805f9b34fb"

CMD_BASIC_INFO = 0x03
CMD_CELL_VOLTAGES = 0x04

# Warning thresholds for the series pack.
SOC_DRIFT_WARN = 5          # % difference between batteries
AVG_CELL_DRIFT_WARN = 0.020  # V difference between the batteries' average cell voltage
CELL_SPREAD_WARN = 0.050     # V between the lowest and highest cell in the whole pack
CURRENT_MISMATCH_WARN = 1.0  # A; in series both should read the same

PROTECTION_FLAGS = [
    "cell overvoltage", "cell undervoltage", "pack overvoltage", "pack undervoltage",
    "charge overtemp", "charge undertemp", "discharge overtemp", "discharge undertemp",
    "charge overcurrent", "discharge overcurrent", "short circuit", "frontend IC error",
    "MOSFET software lock",
]


def build_request(cmd: int) -> bytes:
    # DD A5 <cmd> 00 <checksum hi> <checksum lo> 77
    checksum = (0x10000 - (cmd + 0x00)) & 0xFFFF
    return bytes([0xDD, 0xA5, cmd, 0x00, checksum >> 8, checksum & 0xFF, 0x77])


def u16(d: bytes, i: int) -> int:
    return int.from_bytes(d[i:i + 2], "big")


def s16(d: bytes, i: int) -> int:
    return int.from_bytes(d[i:i + 2], "big", signed=True)


@dataclass
class BasicInfo:
    voltage: float          # V
    current: float          # A, negative = discharging
    remaining_ah: float
    nominal_ah: float
    full_charge_ah: float | None  # only on newer firmware
    cycles: int
    manufactured: str
    balancing: int          # bitmask of cells being balanced
    protection: int         # bitmask, see PROTECTION_FLAGS
    soc: int                # %
    charge_fet: bool
    discharge_fet: bool
    cell_count: int
    temperatures: list[float]

    @classmethod
    def parse(cls, d: bytes) -> BasicInfo:
        date = u16(d, 10)
        ntc = d[22]
        temps = [(u16(d, 23 + 2 * i) - 2731) / 10 for i in range(ntc)]
        ext = 23 + 2 * ntc  # humidity(1), alarm(2), full charge cap(2), remaining(2), balance current(2)
        full = u16(d, ext + 3) / 100 if len(d) >= ext + 5 else None
        return cls(
            voltage=u16(d, 0) / 100,
            current=s16(d, 2) / 100,
            remaining_ah=u16(d, 4) / 100,
            nominal_ah=u16(d, 6) / 100,
            full_charge_ah=full,
            cycles=u16(d, 8),
            manufactured=f"{2000 + (date >> 9)}-{(date >> 5) & 0x0F:02}-{date & 0x1F:02}",
            balancing=u16(d, 12) | (u16(d, 14) << 16),
            protection=u16(d, 16),
            soc=d[19],
            charge_fet=bool(d[20] & 0x01),
            discharge_fet=bool(d[20] & 0x02),
            cell_count=d[21],
            temperatures=temps,
        )

    def protection_names(self) -> list[str]:
        return [n for bit, n in enumerate(PROTECTION_FLAGS) if self.protection & (1 << bit)]


def parse_cells(d: bytes) -> list[float]:
    return [u16(d, i) / 1000 for i in range(0, len(d), 2)]


class FrameAssembler:
    """Reassembles a JBD reply that arrives split across several BLE notifications."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, chunk: bytes) -> list[tuple[int, bytes]]:
        self.buf += chunk
        frames = []
        while True:
            start = self.buf.find(0xDD)
            if start < 0:
                self.buf.clear()
                return frames
            del self.buf[:start]
            if len(self.buf) < 4:
                return frames
            total = 4 + self.buf[3] + 3  # header + payload + checksum + 0x77
            if len(self.buf) < total:
                return frames
            frame = bytes(self.buf[:total])
            del self.buf[:total]
            cmd, status, payload = frame[1], frame[2], frame[4:-3]
            checksum = (0x10000 - sum(frame[2:-3])) & 0xFFFF
            if frame[-1] != 0x77 or checksum != u16(frame, total - 3) or status != 0:
                continue  # corrupt or error reply; the request will time out and retry
            frames.append((cmd, payload))


@dataclass
class Battery:
    device: BLEDevice
    label: str = "?"
    info: BasicInfo | None = None
    cells: list[float] = field(default_factory=list)
    error: str | None = None
    # Last raw reply payloads, passed on as-is in --json mode.
    basic_raw: bytes | None = None
    cells_raw: bytes | None = None

    def __post_init__(self) -> None:
        self.client = BleakClient(self.device, disconnected_callback=self._on_disconnect)
        self.assembler = FrameAssembler()
        self.pending: dict[int, asyncio.Future[bytes]] = {}
        self.write_with_response = True

    def _on_disconnect(self, _client: BleakClient) -> None:
        for fut in self.pending.values():
            if not fut.done():
                fut.set_exception(ConnectionError("disconnected"))

    def _on_notify(self, _char, data: bytearray) -> None:
        for cmd, payload in self.assembler.feed(bytes(data)):
            fut = self.pending.pop(cmd, None)
            if fut and not fut.done():
                fut.set_result(payload)

    async def connect(self) -> None:
        if self.client.is_connected:
            return
        await self.client.connect(timeout=15)
        char = self.client.services.get_characteristic(WRITE_CHAR)
        self.write_with_response = "write-without-response" not in char.properties
        await self.client.start_notify(NOTIFY_CHAR, self._on_notify)
        # Some JBD boards ignore commands for a moment after a fresh connection.
        await asyncio.sleep(1.0)

    async def request(self, cmd: int, timeout: float = 2.0, attempts: int = 3) -> bytes:
        for attempt in range(attempts):
            fut = asyncio.get_running_loop().create_future()
            self.pending[cmd] = fut
            await self.client.write_gatt_char(WRITE_CHAR, build_request(cmd), response=self.write_with_response)
            try:
                return await asyncio.wait_for(fut, timeout)
            except TimeoutError:
                self.assembler = FrameAssembler()  # drop any half-received reply
                if attempt == attempts - 1:
                    raise TimeoutError(f"no reply to command 0x{cmd:02X} after {attempts} tries") from None

    async def poll(self) -> None:
        step = "connect"
        try:
            await self.connect()
            step = "basic info"
            self.basic_raw = await self.request(CMD_BASIC_INFO)
            self.info = BasicInfo.parse(self.basic_raw)
            step = "cell voltages"
            self.cells_raw = await self.request(CMD_CELL_VOLTAGES)
            self.cells = parse_cells(self.cells_raw)
            self.error = None
        except Exception as e:  # keep polling the other battery; reconnect next round
            self.error = f"{step} failed: {type(e).__name__}" + (f": {e}" if str(e) else "")
            self.assembler = FrameAssembler()

    def to_json(self) -> str:
        return json.dumps({
            "label": self.label,
            "address": self.device.address,
            "basic": self.basic_raw.hex() if self.basic_raw else None,
            "cells": self.cells_raw.hex() if self.cells_raw else None,
            "error": self.error,
        })

    async def close(self) -> None:
        if self.client.is_connected:
            await self.client.disconnect()


async def discover(name: str, count: int, timeout: float) -> list[BLEDevice]:
    found: dict[str, BLEDevice] = {}
    done = asyncio.Event()

    def on_adv(device: BLEDevice, adv) -> None:
        if (adv.local_name or device.name or "").startswith(name):
            found[device.address] = device
            if len(found) >= count:
                done.set()

    async with BleakScanner(on_adv):
        try:
            await asyncio.wait_for(done.wait(), timeout)
        except TimeoutError:
            pass
    return list(found.values())


def render(batteries: list[Battery]) -> str:
    lines = []
    for b in batteries:
        if not b.info:
            lines.append(f"Battery {b.label}: {b.error or 'no data yet'}")
            continue
        i = b.info
        temps = ", ".join(f"{t:.1f}" for t in i.temperatures)
        cells = " ".join(f"{c:.3f}" for c in b.cells)
        fets = f"chg {'on' if i.charge_fet else 'OFF'} / dis {'on' if i.discharge_fet else 'OFF'}"
        stale = f"  (stale: {b.error})" if b.error else ""
        lines.append(
            f"Battery {b.label}  [{b.device.address}]{stale}\n"
            f"  {i.voltage:6.2f} V  {i.current:+7.2f} A  {i.soc:3d} %  "
            f"{i.remaining_ah:.2f}/{i.full_charge_ah or i.nominal_ah:.2f} Ah  {temps} °C\n"
            f"  cells: {cells} V   cycles {i.cycles}  made {i.manufactured}  {fets}"
        )
        if i.protection:
            lines.append(f"  PROTECTION: {', '.join(i.protection_names())}")

    ready = [b for b in batteries if b.info and b.cells]
    if len(ready) < 2:
        return "\n".join(lines)

    infos = [b.info for b in ready]
    all_cells = [c for b in ready for c in b.cells]
    voltage = sum(i.voltage for i in infos)
    current = sum(i.current for i in infos) / len(infos)
    remaining = min(i.remaining_ah for i in infos)
    soc = min(i.soc for i in infos)
    spread = max(all_cells) - min(all_cells)
    avg_cells = [sum(b.cells) / len(b.cells) for b in ready]

    lines.append("")
    lines.append("PACK (series)")
    lines.append(
        f"  {voltage:6.2f} V  {current:+7.2f} A  {voltage * current:+8.1f} W  {soc:3d} %  "
        f"{remaining:.2f} Ah  (~{remaining * voltage / 1000:.2f} kWh)"
    )
    runtime = f"~{remaining / -current:.1f} h to empty" if current < -0.1 else "not discharging"
    lines.append(
        f"  cells min {min(all_cells):.3f}  max {max(all_cells):.3f}  spread {spread * 1000:.0f} mV   "
        f"max temp {max(t for i in infos for t in i.temperatures):.1f} °C   {runtime}"
    )

    warnings = []
    soc_drift = max(i.soc for i in infos) - soc
    if soc_drift >= SOC_DRIFT_WARN:
        warnings.append(f"SoC differs by {soc_drift} % (counter drift unless cell voltages differ too)")
    if max(avg_cells) - min(avg_cells) >= AVG_CELL_DRIFT_WARN:
        warnings.append(f"batteries out of balance: avg cell differs by {(max(avg_cells) - min(avg_cells)) * 1000:.0f} mV")
    if spread >= CELL_SPREAD_WARN:
        warnings.append(f"cell spread {spread * 1000:.0f} mV")
    if max(i.current for i in infos) - min(i.current for i in infos) >= CURRENT_MISMATCH_WARN:
        warnings.append("current readings disagree (normal briefly when the load changes)")
    for w in warnings:
        lines.append(f"  ⚠ {w}")
    return "\n".join(lines)


async def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", default="DP04S", help="BLE name prefix to match (default: %(default)s)")
    p.add_argument("--count", type=int, default=2, help="number of batteries in series (default: %(default)s)")
    p.add_argument("--interval", type=float, default=5, help="seconds between polls (default: %(default)s)")
    p.add_argument("--scan-timeout", type=float, default=20)
    p.add_argument("--scan", action="store_true", help="list matching batteries and exit")
    p.add_argument("--once", action="store_true", help="take one reading and exit")
    p.add_argument("--json", action="store_true", help="print one JSON line per battery per poll (raw payloads)")
    args = p.parse_args()
    log = sys.stderr if args.json else sys.stdout

    print(f"Scanning for {args.count} × '{args.name}*' (close the Eco-Worthy app and nRF Connect)…", file=log)
    devices = await discover(args.name, args.count, args.scan_timeout)
    if args.scan:
        for d in devices:
            print(f"  {d.name}  {d.address}")
        return 0
    if len(devices) < args.count:
        print(f"Found {len(devices)}/{args.count}: {[d.address for d in devices]}. "
              "Is another app connected to them?", file=sys.stderr)
        if not devices:
            return 1

    batteries = [Battery(d) for d in devices]
    try:
        for _ in range(3):  # first contact is the flakiest; retry before giving up
            await asyncio.gather(*(b.poll() for b in batteries if not b.info))
            if all(b.info for b in batteries):
                break
        # macOS hides MAC addresses, so label batteries by manufacture date for a stable A/B order.
        batteries.sort(key=lambda b: (b.info is None, b.info.manufactured if b.info else ""))
        for label, b in zip("ABCDEFGH", batteries):
            b.label = label
        while True:
            if args.json:
                for b in batteries:
                    print(b.to_json(), flush=True)
            else:
                if not args.once:
                    os.system("cls" if os.name == "nt" else "clear")
                print(render(batteries))
            if args.once:
                return 0
            await asyncio.sleep(args.interval)
            await asyncio.gather(*(b.poll() for b in batteries))
    finally:
        await asyncio.gather(*(b.close() for b in batteries), return_exceptions=True)


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except (KeyboardInterrupt, BrokenPipeError):  # BrokenPipe: the simulator reading --json closed
        pass
