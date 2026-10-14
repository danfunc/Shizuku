#!/usr/bin/env python3
"""Measure BLE GATT latency (scan discovery, connect, FFF1 read, FFF2 write) from a Shizuku peripheral."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import sys
import time
from typing import Any

# UUID definitions matching Shizuku GATT Stage 1 specification:
# modules/esp_idf_support/firmware/main/esp_ble_stage1.cpp:392-394
BUILD_VERSION_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
THROUGHPUT_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"

# Safety note on GATT 0xFFF2 write:
# In modules/esp_idf_support/firmware/main/esp_ble_stage1.cpp (lines 836-852)
# and modules/esp_idf_support/firmware/idf_reference/main/main.c (lines 44-52),
# write requests to 0xFFF2 require a 1-byte command (value <= 1).
# A command byte of 0x01 starts notification transmission, while 0x00 disables
# notifications (calls ble_npl_callout_stop / sets g_throughput_enabled = false).
# Sending b"\x00" with response=True is therefore completely safe and idempotent:
# if notifications are already disabled, stopping the callout is a harmless no-op
# that produces no side effects while testing round-trip write latency.


def calc_stats(values: list[float]) -> dict[str, float | int | None]:
    """Calculate min, median, p95 (nearest-rank), max, and mean for a list of values."""
    if not values:
        return {
            "count": 0,
            "min": None,
            "median": None,
            "p95": None,
            "max": None,
            "mean": None,
        }
    sorted_v = sorted(values)
    n = len(sorted_v)
    # Nearest rank method for 95th percentile:
    # rank = ceil(0.95 * n) in 1..n, so 0-based index is rank - 1.
    rank_p95 = math.ceil(0.95 * n)
    idx_p95 = max(0, min(n - 1, rank_p95 - 1))
    p95_val = sorted_v[idx_p95]

    if n % 2 == 1:
        med_val = sorted_v[n // 2]
    else:
        med_val = (sorted_v[n // 2 - 1] + sorted_v[n // 2]) / 2.0

    return {
        "count": n,
        "min": sorted_v[0],
        "median": med_val,
        "p95": p95_val,
        "max": sorted_v[-1],
        "mean": sum(sorted_v) / n,
    }


def build_result(
    args: argparse.Namespace,
    address: str | None,
    scan_times: list[float],
    scan_failures: int,
    connect_times: list[float],
    connect_failures: int,
    service_times: list[float],
    service_failures: int,
    fff1_times: list[float],
    fff1_failures: int,
    fff2_times: list[float],
    fff2_failures: int,
    disconnected_at: dict[str, Any] | None,
) -> dict[str, Any]:
    """Assemble latency benchmark metrics and statistics into a dictionary."""
    total_failures = (
        scan_failures
        + connect_failures
        + service_failures
        + fff1_failures
        + fff2_failures
    )
    return {
        "label": args.label,
        "name": args.name,
        "address": address,
        "scan_discovery": {
            "stats": calc_stats(scan_times),
            "failures": scan_failures,
        },
        "connect_total": {
            "stats": calc_stats(connect_times),
            "failures": connect_failures,
        },
        "service_discovery": {
            "stats": calc_stats(service_times),
            "failures": service_failures,
        },
        "fff1_read": {
            "stats": calc_stats(fff1_times),
            "failures": fff1_failures,
        },
        "fff2_write": {
            "stats": calc_stats(fff2_times),
            "failures": fff2_failures,
        },
        "failures": total_failures,
        "disconnected_at": disconnected_at,
    }


def format_ms(val: float | None) -> str:
    """Format a millisecond measurement with 2 decimal places, or '-' if None."""
    if val is None:
        return "-"
    return f"{val:.2f}"


def print_table(result: dict[str, Any]) -> None:
    """Display benchmark results in a human-readable table."""
    print("=" * 86)
    label_str = f" [{result['label']}]" if result.get("label") else ""
    addr_str = result.get("address") or "unknown"
    print(f"BLE Latency Benchmark: {result['name']} ({addr_str}){label_str}")
    print("-" * 86)
    print(
        f"{'Metric':<24} {'Success':>8} {'Failed':>8} "
        f"{'Min(ms)':>9} {'Median(ms)':>11} {'P95(ms)':>9} {'Max(ms)':>9} {'Mean(ms)':>9}"
    )
    print("-" * 86)
    metrics = [
        ("Scan Discovery", result["scan_discovery"]),
        ("Connect Total", result["connect_total"]),
        ("Services Query", result["service_discovery"]),
        ("FFF1 Read", result["fff1_read"]),
        ("FFF2 Write (resp=True)", result["fff2_write"]),
    ]
    for name, m in metrics:
        s = m["stats"]
        print(
            f"{name:<24} "
            f"{s['count']:>8} "
            f"{m['failures']:>8} "
            f"{format_ms(s['min']):>9} "
            f"{format_ms(s['median']):>11} "
            f"{format_ms(s['p95']):>9} "
            f"{format_ms(s['max']):>9} "
            f"{format_ms(s['mean']):>9}"
        )
    print("-" * 86)
    disc = result.get("disconnected_at")
    if disc:
        print(f"Disconnected At: stage='{disc['stage']}', elapsed_ms={disc['elapsed_ms']:.2f}")
    else:
        print("Disconnected At: None")
    print("=" * 86)


def run_selfcheck() -> int:
    """Run self-checks on statistical functions without requiring bleak."""
    # 1. Empty list
    empty_stats = calc_stats([])
    assert empty_stats["count"] == 0, f"Expected count 0, got {empty_stats['count']}"
    assert empty_stats["min"] is None, f"Expected min None, got {empty_stats['min']}"
    assert empty_stats["median"] is None, f"Expected median None, got {empty_stats['median']}"
    assert empty_stats["p95"] is None, f"Expected p95 None, got {empty_stats['p95']}"
    assert empty_stats["max"] is None, f"Expected max None, got {empty_stats['max']}"
    assert empty_stats["mean"] is None, f"Expected mean None, got {empty_stats['mean']}"

    # 2. Single element
    single_stats = calc_stats([42.5])
    assert single_stats["count"] == 1
    assert single_stats["min"] == 42.5
    assert single_stats["median"] == 42.5
    assert single_stats["p95"] == 42.5
    assert single_stats["max"] == 42.5
    assert single_stats["mean"] == 42.5

    # 3. Two elements
    two_stats = calc_stats([10.0, 20.0])
    assert two_stats["count"] == 2
    assert two_stats["min"] == 10.0
    assert two_stats["median"] == 15.0
    # ceil(0.95 * 2) = 2 -> rank 2 -> index 1 -> 20.0
    assert two_stats["p95"] == 20.0
    assert two_stats["max"] == 20.0
    assert two_stats["mean"] == 15.0

    # 4. Odd number of elements (unsorted input)
    odd_stats = calc_stats([30.0, 10.0, 20.0])
    assert odd_stats["count"] == 3
    assert odd_stats["min"] == 10.0
    assert odd_stats["median"] == 20.0
    # ceil(0.95 * 3) = 3 -> rank 3 -> index 2 -> 30.0
    assert odd_stats["p95"] == 30.0
    assert odd_stats["max"] == 30.0
    assert odd_stats["mean"] == 20.0

    # 5. 20 elements (1 to 20)
    v20 = [float(i) for i in range(1, 21)]
    stats20 = calc_stats(v20)
    assert stats20["count"] == 20
    assert stats20["min"] == 1.0
    assert stats20["median"] == 10.5
    # ceil(0.95 * 20) = 19 -> index 18 -> 19.0
    assert stats20["p95"] == 19.0
    assert stats20["max"] == 20.0
    assert stats20["mean"] == 10.5

    # 6. 100 elements (1 to 100)
    v100 = [float(i) for i in range(1, 101)]
    stats100 = calc_stats(v100)
    assert stats100["count"] == 100
    assert stats100["min"] == 1.0
    assert stats100["median"] == 50.5
    # ceil(0.95 * 100) = 95 -> index 94 -> 95.0
    assert stats100["p95"] == 95.0
    assert stats100["max"] == 100.0
    assert stats100["mean"] == 50.5

    # 7. Check result formatting & serialization
    dummy_args = argparse.Namespace(name="SelfCheckDev", label="test", count=100)
    dummy_res = build_result(
        args=dummy_args,
        address="11:22:33:44:55:66",
        scan_times=[12.3],
        scan_failures=0,
        connect_times=[45.6],
        connect_failures=0,
        service_times=[1.2],
        service_failures=0,
        fff1_times=[5.0, 6.0],
        fff1_failures=0,
        fff2_times=[7.0, 8.0],
        fff2_failures=0,
        disconnected_at=None,
    )
    serialized = json.dumps(dummy_res)
    assert "SelfCheckDev" in serialized
    assert dummy_res["failures"] == 0

    print("selfcheck ok")
    return 0


async def run_benchmark(args: argparse.Namespace) -> tuple[dict[str, Any], int]:
    """Perform BLE discovery, connection, and GATT round-trip latency measurements."""
    # Delayed import of bleak so --selfcheck runs in environments without bleak
    from bleak import BleakClient, BleakScanner
    from bleak.backends.device import BLEDevice
    from bleak.backends.scanner import AdvertisementData

    overall_start = time.perf_counter()
    current_stage = "scan"
    disconnected_info: dict[str, Any] | None = None

    def on_disconnect(_client: BleakClient) -> None:
        nonlocal disconnected_info
        if disconnected_info is None and current_stage != "disconnect":
            elapsed = (time.perf_counter() - overall_start) * 1000.0
            disconnected_info = {
                "stage": current_stage,
                "elapsed_ms": round(elapsed, 2),
            }

    scan_times: list[float] = []
    scan_failures = 0
    connect_times: list[float] = []
    connect_failures = 0
    service_times: list[float] = []
    service_failures = 0
    fff1_times: list[float] = []
    fff1_failures = 0
    fff2_times: list[float] = []
    fff2_failures = 0

    device: BLEDevice | None = None
    target_name = args.name

    # 1. Scan / Discovery measurement
    found_event = asyncio.Event()
    matched_device: BLEDevice | None = None
    discovery_time: float | None = None

    def detection_callback(dev: BLEDevice, adv: AdvertisementData) -> None:
        nonlocal matched_device, discovery_time
        name = adv.local_name or dev.name
        if name == target_name and not found_event.is_set():
            discovery_time = time.perf_counter()
            matched_device = dev
            found_event.set()

    t_scan_start = time.perf_counter()
    scanner = BleakScanner(detection_callback=detection_callback)
    try:
        await scanner.start()
        try:
            await asyncio.wait_for(found_event.wait(), timeout=args.scan_timeout)
            if discovery_time is not None:
                scan_times.append((discovery_time - t_scan_start) * 1000.0)
            device = matched_device
        except asyncio.TimeoutError:
            scan_failures += 1
            if disconnected_info is None:
                elapsed = (time.perf_counter() - overall_start) * 1000.0
                disconnected_info = {"stage": "scan", "elapsed_ms": round(elapsed, 2)}
    except Exception:
        scan_failures += 1
        if disconnected_info is None:
            elapsed = (time.perf_counter() - overall_start) * 1000.0
            disconnected_info = {"stage": "scan", "elapsed_ms": round(elapsed, 2)}
    finally:
        try:
            await scanner.stop()
        except Exception:
            pass

    # If device was not discovered, return partial results immediately
    if device is None:
        result = build_result(
            args=args,
            address=None,
            scan_times=scan_times,
            scan_failures=scan_failures,
            connect_times=connect_times,
            connect_failures=connect_failures,
            service_times=service_times,
            service_failures=service_failures,
            fff1_times=fff1_times,
            fff1_failures=fff1_failures,
            fff2_times=fff2_times,
            fff2_failures=fff2_failures,
            disconnected_at=disconnected_info,
        )
        return result, 1

    # 2. Connection establishment
    current_stage = "connect"
    client = BleakClient(
        device,
        timeout=args.timeout,
        disconnected_callback=on_disconnect,
    )
    t_conn_start = time.perf_counter()
    try:
        await client.connect()
        t_conn_end = time.perf_counter()
        connect_times.append((t_conn_end - t_conn_start) * 1000.0)
    except Exception:
        connect_failures += 1
        if disconnected_info is None:
            elapsed = (time.perf_counter() - overall_start) * 1000.0
            disconnected_info = {"stage": "connect", "elapsed_ms": round(elapsed, 2)}
        result = build_result(
            args=args,
            address=device.address,
            scan_times=scan_times,
            scan_failures=scan_failures,
            connect_times=connect_times,
            connect_failures=connect_failures,
            service_times=service_times,
            service_failures=service_failures,
            fff1_times=fff1_times,
            fff1_failures=fff1_failures,
            fff2_times=fff2_times,
            fff2_failures=fff2_failures,
            disconnected_at=disconnected_info,
        )
        return result, 1

    try:
        # Service query / cache check
        t_svc_start = time.perf_counter()
        try:
            _ = client.services
            service_times.append((time.perf_counter() - t_svc_start) * 1000.0)
        except Exception:
            service_failures += 1

        # 3. FFF1 Read (count iterations)
        current_stage = "read_fff1"
        for _ in range(args.count):
            if disconnected_info is not None or not client.is_connected:
                if disconnected_info is None:
                    elapsed = (time.perf_counter() - overall_start) * 1000.0
                    disconnected_info = {"stage": current_stage, "elapsed_ms": round(elapsed, 2)}
                break
            t0 = time.perf_counter()
            try:
                _ = await client.read_gatt_char(BUILD_VERSION_UUID)
                dt = (time.perf_counter() - t0) * 1000.0
                fff1_times.append(dt)
            except Exception:
                fff1_failures += 1
                if not client.is_connected:
                    if disconnected_info is None:
                        elapsed = (time.perf_counter() - overall_start) * 1000.0
                        disconnected_info = {"stage": current_stage, "elapsed_ms": round(elapsed, 2)}
                    break

        # 4. FFF2 Write-with-response (count iterations)
        current_stage = "write_fff2"
        for _ in range(args.count):
            if disconnected_info is not None or not client.is_connected:
                if disconnected_info is None:
                    elapsed = (time.perf_counter() - overall_start) * 1000.0
                    disconnected_info = {"stage": current_stage, "elapsed_ms": round(elapsed, 2)}
                break
            t0 = time.perf_counter()
            try:
                await client.write_gatt_char(THROUGHPUT_UUID, b"\x00", response=True)
                dt = (time.perf_counter() - t0) * 1000.0
                fff2_times.append(dt)
            except Exception:
                fff2_failures += 1
                if not client.is_connected:
                    if disconnected_info is None:
                        elapsed = (time.perf_counter() - overall_start) * 1000.0
                        disconnected_info = {"stage": current_stage, "elapsed_ms": round(elapsed, 2)}
                    break
    finally:
        current_stage = "disconnect"
        try:
            await client.disconnect()
        except Exception:
            pass

    result = build_result(
        args=args,
        address=device.address,
        scan_times=scan_times,
        scan_failures=scan_failures,
        connect_times=connect_times,
        connect_failures=connect_failures,
        service_times=service_times,
        service_failures=service_failures,
        fff1_times=fff1_times,
        fff1_failures=fff1_failures,
        fff2_times=fff2_times,
        fff2_failures=fff2_failures,
        disconnected_at=disconnected_info,
    )

    total_failures = (
        scan_failures
        + connect_failures
        + service_failures
        + fff1_failures
        + fff2_failures
    )
    is_success = (
        total_failures == 0
        and len(scan_times) == 1
        and len(connect_times) == 1
        and len(fff1_times) == args.count
        and len(fff2_times) == args.count
        and disconnected_info is None
    )
    exit_code = 0 if is_success else 1
    return result, exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", help="advertised BLE device name (required unless --selfcheck)")
    parser.add_argument("--count", type=int, default=100, help="iteration count for FFF1 read and FFF2 write (default: 100)")
    parser.add_argument("--scan-timeout", type=float, default=15.0, help="scan timeout in seconds (default: 15.0)")
    parser.add_argument("--timeout", type=float, default=20.0, help="GATT connection timeout in seconds (default: 20.0)")
    parser.add_argument("--json", action="store_true", help="output result as single-line JSON")
    parser.add_argument("--label", type=str, default="", help="optional benchmark run label")
    parser.add_argument("--selfcheck", action="store_true", help="run unit self-check without requiring bleak")

    args = parser.parse_args()

    if args.selfcheck:
        return run_selfcheck()

    if not args.name:
        parser.error("--name is required when not running --selfcheck")

    if args.count <= 0 or args.scan_timeout <= 0 or args.timeout <= 0:
        parser.error("count, scan-timeout, and timeout must be positive")

    try:
        result, exit_code = asyncio.run(run_benchmark(args))
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        parser.exit(1, f"error: {error}\n")

    if args.json:
        print(json.dumps(result, separators=(",", ":")))
    else:
        print_table(result)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
