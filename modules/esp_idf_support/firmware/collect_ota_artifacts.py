#!/usr/bin/env python3
"""Capture and fail-closed validate a resolved C6 BLE OTA build bundle."""
import csv
import glob
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys

if len(sys.argv) != 7:
    raise SystemExit("usage: collect_ota_artifacts.py <bin> <sdkconfig> <bootloader> <partition-bin> <partition-csv> <manifest>")
bin_path = pathlib.Path(os.path.realpath(sys.argv[1]))
out_paths = [pathlib.Path(os.path.realpath(p)) for p in sys.argv[2:6]]
manifest_path = pathlib.Path(os.path.realpath(sys.argv[6]))
build_dirs = [pathlib.Path(p) for p in glob.glob(str(bin_path) + ".idf.*") if pathlib.Path(p).is_dir()]
if not build_dirs:
    raise SystemExit(f"no build.sh work directory found for {bin_path}")
build_dir = max(build_dirs, key=lambda p: p.stat().st_mtime_ns)
inputs = [
    build_dir / "sdkconfig",
    build_dir / "bootloader" / "bootloader.bin",
    build_dir / "partition_table" / "partition-table.bin",
]
for src, dst in zip(inputs, out_paths[:3]):
    if not src.is_file() or src.stat().st_size == 0:
        raise SystemExit(f"required artifact missing or empty: {src}")
    shutil.copyfile(src, dst)

idf_path = pathlib.Path(os.environ.get("IDF_PATH", ""))
partition_decoder = idf_path / "components" / "partition_table" / "gen_esp32part.py"
if not partition_decoder.is_file():
    raise SystemExit(f"pinned IDF partition decoder missing: {partition_decoder}")
decode = subprocess.run(
    [sys.executable, str(partition_decoder), str(out_paths[2]), str(out_paths[3]), "--flash-size=4MB"],
    check=False, text=True, capture_output=True)
if decode.returncode != 0:
    raise SystemExit(f"gen_esp32part.py failed ({decode.returncode}): {decode.stderr.strip()}")
print(decode.stdout.strip())
if decode.stderr.strip():
    print(decode.stderr.strip(), file=sys.stderr)

config = {}
for line in out_paths[0].read_text(encoding="utf-8").splitlines():
    if line.startswith("CONFIG_") and "=" in line:
        key, value = line.split("=", 1)
        config[key] = value
    elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
        key = line[2:].split(" is not set", 1)[0]
        config[key] = "n"
required = {
    "CONFIG_IDF_TARGET": '"esp32c6"',
    "CONFIG_PARTITION_TABLE_CUSTOM": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": '"partitions_esp32c6_ota.csv"',
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
    "CONFIG_BT_NIMBLE_SECURITY_ENABLE": "n",
    "CONFIG_ESPTOOLPY_FLASHSIZE": '"4MB"',
    "CONFIG_ESPTOOLPY_FLASHSIZE_4MB": "y",
}
for key, want in required.items():
    if config.get(key) != want:
        raise SystemExit(f"resolved sdkconfig mismatch: {key}={config.get(key)!r}, expected {want!r}")
try:
    table_offset = int(config["CONFIG_PARTITION_TABLE_OFFSET"], 0)
except (KeyError, ValueError) as exc:
    raise SystemExit(f"resolved table offset unavailable: {exc}")
flash_size = 0x400000 if config["CONFIG_ESPTOOLPY_FLASHSIZE"] == '"4MB"' else 0
if flash_size != 0x400000 or table_offset != 0x8000:
    raise SystemExit(f"flash/table bounds mismatch: flash={flash_size:#x}, table={table_offset:#x}")

partitions = {}
with out_paths[3].open(newline="", encoding="utf-8") as stream:
    rows = csv.reader(line for line in stream if not line.lstrip().startswith("#"))
    for row in rows:
        if not row or not row[0].strip():
            continue
        name, ptype, subtype, offset, size = [cell.strip() for cell in row[:5]]
        def parse_size(text):
            suffix = text[-1:].upper()
            factor = {"K": 1024, "M": 1024 * 1024}.get(suffix, 1)
            digits = text[:-1] if factor != 1 else text
            return int(digits, 0) * factor
        partitions[name] = (ptype, subtype, int(offset, 0), parse_size(size))
expected = {
    "nvs": ("data", "nvs", 0x9000, 24 * 1024),
    "otadata": ("data", "ota", 0xF000, 0x2000),
    "phy_init": ("data", "phy", 0x11000, 0x1000),
    "ota_0": ("app", "ota_0", 0x20000, 0x1F0000),
    "ota_1": ("app", "ota_1", 0x210000, 0x1F0000),
}
if partitions != expected:
    raise SystemExit(f"decoded partition table mismatch: {partitions!r}")
for name in ("ota_0", "ota_1"):
    _, _, offset, size = partitions[name]
    if offset % 0x10000 or size % 0x10000 or offset + size > 0x400000:
        raise SystemExit(f"invalid OTA app partition bounds: {name} offset={offset:#x} size={size:#x}")
if bin_path.stat().st_size > min(partitions["ota_0"][3], partitions["ota_1"][3]):
    raise SystemExit("application binary exceeds an OTA slot")

elf_path = bin_path.with_suffix(".elf")
map_path = bin_path.with_suffix(".map")
files = [bin_path, elf_path, map_path, *out_paths]
for path in files:
    if not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"bundle artifact missing or empty: {path}")
manifest = {
    "profile": "esp32c6_ble_ota",
    "build_dir": str(build_dir),
    "flash_bytes": flash_size,
    "partition_table_offset": table_offset,
    "partitions": {name: {"type": row[0], "subtype": row[1],
                           "offset": row[2], "size": row[3]}
                   for name, row in partitions.items()},
    "resolved_config": {key: config[key] for key in required},
    "artifacts": {
        p.name: {"size": p.stat().st_size,
                 "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in files
    },
}
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"verified_profile": manifest["profile"],
                  "app_bytes": bin_path.stat().st_size,
                  "slot_bytes": partitions["ota_0"][3],
                  "artifacts": len(files)}, sort_keys=True))
