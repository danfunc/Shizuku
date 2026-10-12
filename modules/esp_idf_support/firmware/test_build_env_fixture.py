#!/usr/bin/env python3
"""Host mock for relative ESP-IDF paths and fail-closed post-export checks."""
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
