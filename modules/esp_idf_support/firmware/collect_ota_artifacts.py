#!/usr/bin/env python3
"""Copy the resolved C6 OTA build inputs produced beside build.sh's BIN."""
import glob
import hashlib
import json
import os
import pathlib
import shutil
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
    build_dir / "partition_table" / "partition-table.csv",
]
for src, dst in zip(inputs, out_paths):
    if not src.is_file() or src.stat().st_size == 0:
        raise SystemExit(f"required artifact missing or empty: {src}")
    shutil.copyfile(src, dst)
files = [bin_path, *out_paths]
manifest = {
    "profile": "esp32c6_ble_ota",
    "build_dir": str(build_dir),
    "artifacts": {
        p.name: {"size": p.stat().st_size,
                 "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in files
    },
}
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
