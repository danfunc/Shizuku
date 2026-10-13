#!/usr/bin/env python3
"""Measure GATT notification throughput from a Shizuku BLE peripheral."""

from __future__ import annotations

import argparse
import asyncio
import time

from bleak import BleakClient, BleakScanner


THROUGHPUT_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"
BUILD_VERSION_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"


async def run(args: argparse.Namespace) -> int:
    device = await BleakScanner.find_device_by_name(
        args.name, timeout=args.scan_timeout
    )
    if device is None:
        raise RuntimeError(f"BLE device {args.name!r} was not found")

    packet_count = 0
    payload_bytes = 0
    missing_packets = 0
    duplicate_packets = 0
    out_of_order_packets = 0
    malformed_packets = 0
    previous_sequence: int | None = None
    first_sequence: int | None = None
    last_sequence: int | None = None
    payload_lengths: dict[int, int] = {}

    def on_notification(_characteristic: object, data: bytearray) -> None:
        nonlocal packet_count, payload_bytes, missing_packets
        nonlocal duplicate_packets, out_of_order_packets, malformed_packets
        nonlocal previous_sequence, first_sequence, last_sequence
        packet_count += 1
        payload_bytes += len(data)
        payload_lengths[len(data)] = payload_lengths.get(len(data), 0) + 1
        if len(data) < 4:
            malformed_packets += 1
            return
        sequence = int.from_bytes(data[:4], "little")
        if first_sequence is None:
            first_sequence = sequence
        if previous_sequence is not None:
            delta = (sequence - previous_sequence) & 0xFFFFFFFF
            if delta == 0:
                duplicate_packets += 1
            elif delta < 0x80000000:
                missing_packets += delta - 1
            else:
                out_of_order_packets += 1
        previous_sequence = sequence
        last_sequence = sequence

    async with BleakClient(device, timeout=args.timeout) as client:
        version = bytes(await client.read_gatt_char(BUILD_VERSION_UUID))
        if not version:
            raise RuntimeError("GATT 0xFFF1 returned an empty build version")
        print(f"device={args.name} address={device.address}")
        print(f"gatt_fff1_hex={version.hex()} build_version={version.decode('utf-8', errors='replace')!r}")
        mtu_start = client.mtu_size
        await client.start_notify(THROUGHPUT_UUID, on_notification)
        started = time.monotonic()
        await client.write_gatt_char(THROUGHPUT_UUID, b"\x01", response=True)
        await asyncio.sleep(args.duration)
        await client.write_gatt_char(THROUGHPUT_UUID, b"\x00", response=True)
        elapsed = time.monotonic() - started
        await client.stop_notify(THROUGHPUT_UUID)
        mtu_end = client.mtu_size

    print(f"att_mtu_start={mtu_start} att_mtu_end={mtu_end}")
    # Bleak's public client API has no negotiated-PHY query. Obtain the actual
    # TX/RX PHY from the peripheral GAP PHY-update diagnostics, not its build
    # preference: a preference alone does not establish the negotiated PHY.
    print("phy=unknown source=peripheral_gap_diagnostics_required")
    print("payload_length_counts=" + ",".join(
        f"{length}:{count}" for length, count in sorted(payload_lengths.items())
    ))
    print(f"elapsed_s={elapsed:.3f} packets={packet_count}")
    print(f"payload_bytes={payload_bytes} bytes_per_second={payload_bytes / elapsed:.1f}")
    print(f"packets_per_second={packet_count / elapsed:.1f}")
    print(
        "sequence="
        f"first:{first_sequence if first_sequence is not None else 'none'} "
        f"last:{last_sequence if last_sequence is not None else 'none'} "
        f"missing:{missing_packets} duplicates:{duplicate_packets} "
        f"out_of_order:{out_of_order_packets} malformed:{malformed_packets}"
    )
    if packet_count == 0 or malformed_packets:
        raise RuntimeError("no valid throughput measurement: empty or malformed notifications")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="Shizuku-C6", help="advertised BLE device name")
    parser.add_argument("--duration", type=float, default=10.0, help="receive duration in seconds")
    parser.add_argument("--scan-timeout", type=float, default=15.0)
    parser.add_argument("--timeout", type=float, default=20.0, help="GATT connection timeout")
    args = parser.parse_args()
    if args.duration <= 0 or args.scan_timeout <= 0 or args.timeout <= 0:
        parser.error("duration and timeouts must be positive")
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
