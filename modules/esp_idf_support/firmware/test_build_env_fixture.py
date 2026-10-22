#!/usr/bin/env python3
"""Host mock for relative ESP-IDF paths and fail-closed post-export checks."""
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


BUILD_SH = Path(__file__).with_name("build.sh")


def executable(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)
    path.chmod(0o755)


class BuildEnvironmentFixtureTest(unittest.TestCase):
    def test_relative_sdk_paths_survive_cwd_change_and_missing_python_fails(self):
        with tempfile.TemporaryDirectory(prefix="shizuku-build-env-") as temp:
            root = Path(temp)
            idf = root / "mock-idf"
            tools = root / "mock-tools"
            bin_dir = tools / "bin"
            python_dir = tools / "python_env" / "bin"
            (idf / "tools" / "cmake").mkdir(parents=True)
            (idf / "tools" / "cmake" / "project.cmake").write_text("# fixture\n")
            env_log = root / "export-env.log"
            executable(idf / "export.sh", '''
export PATH="$IDF_TOOLS_PATH/python_env/bin:$IDF_TOOLS_PATH/bin:$PATH"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env"
printf '%s\\n%s\\n' "$IDF_PATH" "$IDF_TOOLS_PATH" > "$MOCK_ENV_LOG"
''')
            executable(python_dir / "python", "#!/bin/sh\nexit 0\n")
            executable(bin_dir / "riscv32-esp-elf-gcc", "#!/bin/sh\necho 'riscv32-esp-elf-gcc esp-15.2.0_20251204'\n")
            executable(bin_dir / "idf.py", '''#!/bin/sh
if [ "$1" = "--version" ]; then echo "ESP-IDF v6.1"; exit 0; fi
build_dir=""
args="$*"
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-B" ]; then shift; build_dir="$1"; fi
  shift
done
case "$args" in
  *build*) mkdir -p "$build_dir"; printf '\\351fixture' > "$build_dir/shizuku_esp32c6.bin" ;;
esac
''')
            archive = root / "kernel.a"
            archive.write_bytes(b"fixture archive")
            env = os.environ.copy()
            env.update({
                "IDF_PATH": "mock-idf",
                "IDF_TOOLS_PATH": "mock-tools",
                "MOCK_ENV_LOG": str(env_log),
                "PATH": f"{os.environ['PATH']}:{bin_dir}",
            })
            positive = subprocess.run(
                ["bash", str(BUILD_SH), "kernel.a", "out/image.bin", "esp-15.2.0_20251204"],
                cwd=root, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertEqual(positive.returncode, 0, positive.stderr + positive.stdout)
            self.assertEqual(env_log.read_text().splitlines(), [str(idf.resolve()), str(tools.resolve())])
            self.assertEqual((root / "out" / "image.bin").read_bytes(), b"\xe9fixture")

            negative_env = env.copy()
            negative_env["MOCK_NO_PYTHON"] = "1"
            executable(idf / "export.sh", '''
if [ "${MOCK_NO_PYTHON:-}" = "1" ]; then export PATH="$IDF_TOOLS_PATH/bin"; else export PATH="$IDF_TOOLS_PATH/python_env/bin:$IDF_TOOLS_PATH/bin:$PATH"; fi
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env"
''')
            negative = subprocess.run(
                ["bash", str(BUILD_SH), "kernel.a", "out/missing-python.bin", "esp-15.2.0_20251204"],
                cwd=root, env=negative_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            self.assertNotEqual(negative.returncode, 0)
            self.assertIn("export.sh 後に python が PATH 上に無い", negative.stderr)
            self.assertFalse((root / "out" / "missing-python.bin").exists())

    def test_ota_profile_forwards_external_inputs_and_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="shizuku-ota-inputs-") as temp:
            root = Path(temp)
            idf = root / "mock-idf"
            tools = root / "mock-tools"
            tools_bin = tools / "bin"
            python_bin = tools / "python_env" / "bin"
            idf_bin = idf / "bin"
            log_path = root / "idf-args.jsonl"
            executable(idf / "components" / "partition_table" / "gen_esp32part.py", '''#!/usr/bin/env python3
import os
import pathlib
import sys

binary, csv_path, flash_size = sys.argv[1:]
assert pathlib.Path(binary).is_file()
assert flash_size == "--flash-size=4MB"
# Stand-in for the decoder: emit the rows of the real partition table source (MOCK_PARTITION_CSV).
rows = [line for line in pathlib.Path(os.environ["MOCK_PARTITION_CSV"]).read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")]
pathlib.Path(csv_path).write_text("".join(",".join(cell.strip() for cell in row.split(",")) + "\\n" for row in rows))
''')
            executable(idf / "export.sh", '''
export PATH="$MOCK_IDF/bin:$IDF_TOOLS_PATH/bin:$IDF_TOOLS_PATH/python_env/bin:$PATH"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/python_env"
''')
            executable(tools_bin / "riscv32-esp-elf-gcc", "#!/bin/sh\necho 'riscv32-esp-elf-gcc esp-15.2.0_20251204'\n")
            executable(python_bin / "python", "#!/bin/sh\nexec python3 \"$@\"\n")
            executable(idf_bin / "idf.py", '''#!/usr/bin/env python3
import json
import os
import pathlib
import sys

args = sys.argv[1:]
with open(os.environ["MOCK_IDF_ARGS"], "a", encoding="utf-8") as log:
    log.write(json.dumps(args) + "\\n")
if args == ["--version"]:
    print("ESP-IDF v6.1")
    raise SystemExit(0)
build_dir = pathlib.Path(args[args.index("-B") + 1])
sdkconfig = pathlib.Path(next(arg.split("=", 1)[1] for arg in args
                              if arg.startswith("-DSDKCONFIG=")))
if "set-target" in args:
    # Resolve the sdkconfig the way IDF does for this profile: CONFIG_IDF_TARGET comes from set-target,
    # the rest from the defaults files named in MOCK_SDKCONFIG_DEFAULTS; "=n" becomes "is not set".
    target = args[args.index("set-target") + 1]
    lines = ['CONFIG_IDF_TARGET="' + target + '"']
    for defaults in os.environ["MOCK_SDKCONFIG_DEFAULTS"].split(";"):
        for line in pathlib.Path(defaults).read_text().splitlines():
            if not line.startswith("CONFIG_"):
                continue
            key, value = line.split("=", 1)
            lines.append("# " + key + " is not set" if value == "n" else line)
    if "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y" in lines:
        lines.append('CONFIG_ESPTOOLPY_FLASHSIZE="4MB"')
    build_dir.mkdir(parents=True, exist_ok=True)
    sdkconfig.write_text("\\n".join(lines) + "\\n")
if "build" in args:
    (build_dir / "bootloader").mkdir(parents=True, exist_ok=True)
    (build_dir / "partition_table").mkdir(parents=True, exist_ok=True)
    (build_dir / "shizuku_esp32c6.bin").write_bytes(b"\\351fixture")
    (build_dir / "shizuku_esp32c6.elf").write_bytes(b"elf fixture")
    (build_dir / "shizuku_esp32c6.map").write_bytes(b"map fixture")
    (build_dir / "bootloader" / "bootloader.bin").write_bytes(b"boot fixture")
    (build_dir / "partition_table" / "partition-table.bin").write_bytes(b"partition fixture")
''')
            source_dir = root / "xno" / "objects" / "ota"
            source_dir.mkdir(parents=True)
            source_paths = [source_dir / "ota.cpp", source_dir / "protocol.cpp"]
            for source in source_paths:
                source.write_text("// fixture source\\n")
            include_dir = source_dir / "include"
            include_dir.mkdir()
            archive = root / "kernel.a"
            archive.write_bytes(b"fixture archive")
            env = os.environ.copy()
            env.update({
                "IDF_PATH": str(idf),
                "IDF_TOOLS_PATH": str(tools),
                "MOCK_IDF": str(idf),
                "MOCK_IDF_ARGS": str(log_path),
                # The same defaults CMakeLists.txt:27-28 loads for SHIZUKU_ESP_OTA=1, and the real partition table.
                "MOCK_SDKCONFIG_DEFAULTS": ";".join(str(Path(__file__).with_name(name)) for name in (
                    "sdkconfig.defaults.esp32c6.ble", "esp32c6_ota_defaults.conf")),
                "MOCK_PARTITION_CSV": str(Path(__file__).with_name("partitions_esp32c6_ota.csv")),
            })

            def run_profile(name, inputs):
                output_dir = root / name
                output_dir.mkdir()
                paths = {key: output_dir / filename for key, filename in {
                    "bin": "image.bin",
                    "elf": "image.elf",
                    "map": "image.map",
                    "sdkconfig": "sdkconfig",
                    "bootloader": "bootloader.bin",
                    "partition_bin": "partition-table.bin",
                    "partition_csv": "partition-table.csv",
                    "manifest": "manifest.json",
                }.items()}
                command = [
                    "bash", str(Path(__file__).with_name("build_ota_profile.sh")),
                    str(BUILD_SH), str(Path(__file__).with_name("collect_ota_artifacts.py")),
                    str(archive), str(paths["bin"]), str(paths["elf"]), str(paths["map"]),
                    str(paths["sdkconfig"]), str(paths["bootloader"]),
                    str(paths["partition_bin"]), str(paths["partition_csv"]),
                    str(paths["manifest"]), *inputs,
                ]
                result = subprocess.run(
                    command, cwd=root, env=env, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )
                return result, paths

            options = ["--ota-sources", *[str(path.relative_to(root)) for path in source_paths],
                       "--ota-include-dir", str(include_dir.relative_to(root))]
            success, artifacts = run_profile("success", options)
            self.assertEqual(success.returncode, 0, success.stderr + success.stdout)
            calls = [json.loads(line) for line in log_path.read_text().splitlines()]
            build_calls = [args for args in calls if "set-target" in args or "build" in args]
            self.assertEqual(len(build_calls), 2)
            source_arg = "-DSHIZUKU_ESP_OTA_SOURCES=" + ";".join(
                str(path.resolve()) for path in source_paths
            )
            include_arg = "-DSHIZUKU_ESP_OTA_INCLUDE_DIRS=" + str(include_dir.resolve())
            for args in build_calls:
                self.assertIn(source_arg, args)
                self.assertIn(include_arg, args)
            self.assertEqual(artifacts["bin"].read_bytes(), b"\351fixture")
            self.assertEqual(artifacts["elf"].read_bytes(), b"elf fixture")
            self.assertEqual(artifacts["map"].read_bytes(), b"map fixture")
            self.assertIn('CONFIG_IDF_TARGET="esp32c6"', artifacts["sdkconfig"].read_text())
            self.assertEqual(artifacts["bootloader"].read_bytes(), b"boot fixture")
            self.assertIn("ota_1,app,ota_1", artifacts["partition_csv"].read_text())
            self.assertEqual(json.loads(artifacts["manifest"].read_text())["profile"], "esp32c6_ble_ota")
            self.assertFalse((root / "shizuku_esp32c6.elf").exists())
            self.assertFalse((root / "shizuku_esp32c6.map").exists())
            idf_call_count = len(log_path.read_text().splitlines())

            missing_source, missing_source_artifacts = run_profile(
                "missing-source", ["--ota-sources", str(source_dir / "absent.cpp"),
                                   "--ota-include-dir", str(include_dir)],
            )
            self.assertNotEqual(missing_source.returncode, 0)
            self.assertIn("外部 OTA source が無い", missing_source.stderr)
            self.assertFalse(missing_source_artifacts["bin"].exists())
            self.assertEqual(len(log_path.read_text().splitlines()), idf_call_count)

            missing_include, missing_include_artifacts = run_profile(
                "missing-include", ["--ota-sources", str(source_paths[0]),
                                    "--ota-include-dir", str(source_dir / "absent")],
            )
            self.assertNotEqual(missing_include.returncode, 0)
            self.assertIn("外部 OTA include dir が無い", missing_include.stderr)
            self.assertFalse(missing_include_artifacts["bin"].exists())
            self.assertEqual(len(log_path.read_text().splitlines()), idf_call_count)

    def test_ota_component_handles_idf_early_requirements_phase(self):
        component_source = Path(__file__).resolve().parents[1] / "ble_ota" / "CMakeLists.txt"
        self.assertTrue(component_source.is_file())
        with tempfile.TemporaryDirectory(prefix="shizuku-ota-cmake-phase-") as temp:
            root = Path(temp)
            project = root / "project"
            component_dir = project / "modules" / "esp_idf_support" / "ble_ota"
            component_dir.mkdir(parents=True)
            (component_dir / "CMakeLists.txt").write_text(component_source.read_text())
            (project / "internal_headers").mkdir(parents=True)
            (project / "modules" / "pico_sdk_support" / "objects" / "internal_headers").mkdir(parents=True)
            xno_dir = root / "xno" / "objects" / "ota"
            xno_dir.mkdir(parents=True)
            sources = [xno_dir / "auth_gate.cpp", xno_dir / "protocol.cpp", xno_dir / "status_format.cpp"]
            for source in sources:
                source.write_text("// CMake phase fixture\n")
            include_dir = xno_dir

            def run_case(name, setup):
                script = root / f"{name}.cmake"
                script.write_text(f'''function(idf_component_register)
  set(args ${{ARGN}})
  if(MODE STREQUAL "early")
    list(FIND args "REQUIRES" requires_index)
    list(FIND args "SRCS" sources_index)
    if(requires_index EQUAL -1 OR NOT sources_index EQUAL -1)
      message(FATAL_ERROR "early phase must register requirements without source inputs")
    endif()
  elseif(MODE STREQUAL "normal")
    list(FIND args "{sources[1]}" external_source_index)
    list(FIND args "{include_dir}" external_include_index)
    list(FIND args "esp_ble_service.cpp" esp_service_index)
    if(external_source_index EQUAL -1 OR external_include_index EQUAL -1 OR esp_service_index EQUAL -1)
      message(FATAL_ERROR "normal phase args='${{args}}' did not include source='${sources[1]}' include='${include_dir}' esp_ble_service.cpp")
    endif()
  endif()
endfunction()
{setup}
include("{component_dir / 'CMakeLists.txt'}")
''')
                return subprocess.run(
                    ["cmake", "-P", str(script)], text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                )

            early = run_case("early", 'set(MODE "early")\nset(CMAKE_BUILD_EARLY_EXPANSION 1)')
            self.assertEqual(early.returncode, 0, early.stderr + early.stdout)

            full_setup = "\n".join([
                'set(MODE "normal")',
                f'set(SHIZUKU_ESP_OTA_SOURCES "{";".join(map(str, sources))}")',
                f'set(SHIZUKU_ESP_OTA_INCLUDE_DIRS "{include_dir}")',
            ])
            normal = run_case("normal", full_setup)
            self.assertEqual(normal.returncode, 0, normal.stderr + normal.stdout)

            missing_sources = run_case(
                "missing-sources",
                f'set(SHIZUKU_ESP_OTA_INCLUDE_DIRS "{include_dir}")',
            )
            self.assertNotEqual(missing_sources.returncode, 0)
            self.assertIn("requires external XNO sources", missing_sources.stderr)

            missing_include = run_case(
                "missing-include",
                f'set(SHIZUKU_ESP_OTA_SOURCES "{sources[1]}")',
            )
            self.assertNotEqual(missing_include.returncode, 0)
            self.assertIn("requires external XNO include dirs", missing_include.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
