#!/usr/bin/env python3
"""Build-free mock regression test for ESP32-C6 / C5 build.sh project directory resolution.

Verifies:
1. When build.sh is executed from an exec directory (e.g. bazel-out/.../bin/...) lacking CMakeLists.txt,
   invocation without project dir argument fails immediately with a clear diagnostic.
2. Invocation with positional argument 6 (project directory) succeeds, resolves absolute path,
   validates CMakeLists.txt and main/, changes working directory to the project directory,
   and copies .bin, .elf, and .map to outputs.
3. Invocation with SHIZUKU_IDF_PROJECT_DIR / IDF_PROJECT_PATH environment fallback succeeds.
4. Legacy invocation from project directory without argument 6 succeeds using dirname($0).
5. Non-existent project directory or directory missing main/ fails cleanly with clear diagnostics.
6. C5 target is supported and outputs are properly named and copied.
7. defs.bzl statically declares and passes the project directory.
"""

import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_SH_SRC = REPO_ROOT / "modules" / "esp_idf_support" / "firmware" / "build.sh"
DEFS_BZL_SRC = REPO_ROOT / "defs.bzl"


def make_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class C6BuildShMockRegressionTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(BUILD_SH_SRC.exists(), f"build.sh not found: {BUILD_SH_SRC}")
        self.assertTrue(DEFS_BZL_SRC.exists(), f"defs.bzl not found: {DEFS_BZL_SRC}")

    def _setup_mock_idf_environment(self, root: Path):
        idf_path = root / "mock_idf"
        tools_path = root / "mock_tools"
        bin_dir = tools_path / "bin"
        py_dir = tools_path / "python_env" / "bin"

        # Mock export.sh
        make_executable(
            idf_path / "export.sh",
            """#!/bin/sh
export PATH="{bin_dir}:{py_dir}:$PATH"
export IDF_PYTHON_ENV_PATH="{tools_path}/python_env"
""".format(
                bin_dir=bin_dir,
                py_dir=py_dir,
                tools_path=tools_path,
            ),
        )

        # Mock python in IDF python env
        make_executable(py_dir / "python", "#!/bin/sh\nexit 0\n")

        # Mock gcc with expected banner
        make_executable(
            bin_dir / "riscv32-esp-elf-gcc",
            """#!/bin/sh
echo "riscv32-esp-elf-gcc (crosstool-NG esp-15.2.0_20251204) 15.2.0"
""",
        )

        # Mock idf.py
        pwd_log = root / "idf_pwd.log"
        make_executable(
            bin_dir / "idf.py",
            """#!/bin/sh
if [ "$1" = "--version" ]; then
    echo "ESP-IDF v6.1"
    exit 0
fi

pwd > "{pwd_log}"

build_dir=""
target="esp32c6"
args="$*"
while [ "$#" -gt 0 ]; do
    case "$1" in
        -B)
            shift
            build_dir="$1"
            ;;
        -DIDF_TARGET=*)
            target="${{1#-DIDF_TARGET=}}"
            ;;
        set-target)
            shift
            target="$1"
            ;;
    esac
    shift
done

if [ -n "$build_dir" ]; then
    mkdir -p "$build_dir"
    printf '\\351bin_data' > "$build_dir/shizuku_$target.bin"
    printf 'elf_data' > "$build_dir/shizuku_$target.elf"
    printf 'map_data' > "$build_dir/shizuku_$target.map"
fi
exit 0
""".format(pwd_log=pwd_log),
        )

        # Mock project directory (contains CMakeLists.txt and main/)
        project_dir = root / "modules" / "esp_idf_support" / "firmware"
        (project_dir / "main").mkdir(parents=True, exist_ok=True)
        (project_dir / "CMakeLists.txt").write_text("# mock CMakeLists.txt\n", encoding="utf-8")
        (project_dir / "main" / "early_diag.c").write_text("/* mock */\n", encoding="utf-8")

        # Mock exec directory (simulating bazel-out/.../bin/... where build_sh is deployed)
        exec_dir = root / "bazel-out" / "k8-opt-exec-ST-mock" / "bin" / "modules" / "esp_idf_support" / "firmware"
        exec_dir.mkdir(parents=True, exist_ok=True)
        exec_build_sh = exec_dir / "build_sh"
        exec_build_sh.write_bytes(BUILD_SH_SRC.read_bytes())
        exec_build_sh.chmod(exec_build_sh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        # Mock archive input
        archive_path = root / "libkernel.a"
        archive_path.write_bytes(b"!<arch>\nmock_archive")

        env = os.environ.copy()
        env.update(
            {
                "IDF_PATH": str(idf_path),
                "IDF_TOOLS_PATH": str(tools_path),
                "PATH": f"{bin_dir}:{py_dir}:{os.environ.get('PATH', '')}",
            }
        )
        # Ensure clean state without ambient overrides
        env.pop("SHIZUKU_IDF_PROJECT_DIR", None)
        env.pop("IDF_PROJECT_PATH", None)
        env.pop("SHIZUKU_IDF_BUILD_DIR", None)

        return {
            "root": root,
            "idf_path": idf_path,
            "tools_path": tools_path,
            "project_dir": project_dir,
            "exec_dir": exec_dir,
            "exec_build_sh": exec_build_sh,
            "archive_path": archive_path,
            "pwd_log": pwd_log,
            "env": env,
        }

    def test_01_missing_project_dir_arg_fails_from_exec_dir(self):
        """Simulate rbuild failure: running from exec dir without project dir arg must fail cleanly."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "sample.bin"

            # 5 args: ARCHIVE, OUT, WANT_GCC, TARGET, BUILD_DIR (no 6th argument, no env var)
            cmd = [
                str(ctx["exec_build_sh"]),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c6",
                "",
            ]
            res = subprocess.run(cmd, env=ctx["env"], capture_output=True, text=True, cwd=ctx["root"])

            self.assertNotEqual(res.returncode, 0, "Expected failure when project dir is not passed")
            self.assertIn("IDF project directory does not contain CMakeLists.txt", res.stderr)
            self.assertIn(str(ctx["exec_dir"].resolve()), res.stderr)
            self.assertFalse(out_bin.exists(), "Output binary must not be created on failure")

    def test_02_explicit_project_dir_arg_succeeds_from_exec_dir(self):
        """Passing project dir as 6th arg must resolve correctly and build all 3 outputs."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "sample.bin"
            out_elf = ctx["root"] / "out" / "sample.elf"
            out_map = ctx["root"] / "out" / "sample.map"

            cmd = [
                str(ctx["exec_build_sh"]),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c6",
                "",
                str(ctx["project_dir"]),
            ]
            res = subprocess.run(cmd, env=ctx["env"], capture_output=True, text=True, cwd=ctx["root"])

            self.assertEqual(res.returncode, 0, f"Expected success but got {res.returncode}: {res.stderr}\n{res.stdout}")
            self.assertTrue(out_bin.exists(), "Output .bin must exist")
            self.assertTrue(out_elf.exists(), "Output .elf must exist")
            self.assertTrue(out_map.exists(), "Output .map must exist")

            # Verify idf.py ran from the project directory
            self.assertTrue(ctx["pwd_log"].exists(), "idf.py should have written pwd_log")
            actual_pwd = ctx["pwd_log"].read_text(encoding="utf-8").strip()
            self.assertEqual(actual_pwd, str(ctx["project_dir"].resolve()))

    def test_03_env_var_fallback_project_dir_succeeds(self):
        """SHIZUKU_IDF_PROJECT_DIR environment variable provides fallback when arg 6 is omitted."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "sample.bin"

            env = ctx["env"].copy()
            env["SHIZUKU_IDF_PROJECT_DIR"] = str(ctx["project_dir"])

            cmd = [
                str(ctx["exec_build_sh"]),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c6",
                "",
            ]
            res = subprocess.run(cmd, env=env, capture_output=True, text=True, cwd=ctx["root"])

            self.assertEqual(res.returncode, 0, f"Expected success with env var fallback: {res.stderr}")
            self.assertTrue(out_bin.exists())
            actual_pwd = ctx["pwd_log"].read_text(encoding="utf-8").strip()
            self.assertEqual(actual_pwd, str(ctx["project_dir"].resolve()))

    def test_04_legacy_invocation_without_arg6_uses_dirname_0_when_adjacent(self):
        """Legacy genrule invocation directly from project dir without arg 6 succeeds."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "sample.bin"

            # Deploy build.sh into project_dir as legacy genrules do
            legacy_build_sh = ctx["project_dir"] / "build.sh"
            legacy_build_sh.write_bytes(BUILD_SH_SRC.read_bytes())
            legacy_build_sh.chmod(legacy_build_sh.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

            cmd = [
                str(legacy_build_sh),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c6",
            ]
            res = subprocess.run(cmd, env=ctx["env"], capture_output=True, text=True, cwd=ctx["root"])

            self.assertEqual(res.returncode, 0, f"Expected success for legacy invocation: {res.stderr}")
            self.assertTrue(out_bin.exists())
            actual_pwd = ctx["pwd_log"].read_text(encoding="utf-8").strip()
            self.assertEqual(actual_pwd, str(ctx["project_dir"].resolve()))

    def test_05_nonexistent_project_dir_fails(self):
        """Pointing arg 6 to non-existent directory must fail cleanly."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "sample.bin"

            cmd = [
                str(ctx["exec_build_sh"]),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c6",
                "",
                str(ctx["root"] / "nonexistent_dir"),
            ]
            res = subprocess.run(cmd, env=ctx["env"], capture_output=True, text=True, cwd=ctx["root"])

            self.assertNotEqual(res.returncode, 0)
            self.assertIn("IDF project directory does not contain CMakeLists.txt", res.stderr)
            self.assertFalse(out_bin.exists())

    def test_06_project_dir_missing_main_fails(self):
        """Pointing arg 6 to directory containing CMakeLists.txt but missing main/ must fail cleanly."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "sample.bin"

            bad_project = ctx["root"] / "bad_project"
            bad_project.mkdir()
            (bad_project / "CMakeLists.txt").write_text("# mock", encoding="utf-8")

            cmd = [
                str(ctx["exec_build_sh"]),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c6",
                "",
                str(bad_project),
            ]
            res = subprocess.run(cmd, env=ctx["env"], capture_output=True, text=True, cwd=ctx["root"])

            self.assertNotEqual(res.returncode, 0)
            self.assertIn("IDF project directory does not contain main/", res.stderr)
            self.assertFalse(out_bin.exists())

    def test_07_c5_target_support_and_output_naming(self):
        """Target esp32c5 is supported and generates matching outputs."""
        with tempfile.TemporaryDirectory(prefix="shizuku_c6_mock_") as temp:
            ctx = self._setup_mock_idf_environment(Path(temp))
            out_bin = ctx["root"] / "out" / "c5_sample.bin"
            out_elf = ctx["root"] / "out" / "c5_sample.elf"
            out_map = ctx["root"] / "out" / "c5_sample.map"

            cmd = [
                str(ctx["exec_build_sh"]),
                str(ctx["archive_path"]),
                str(out_bin),
                "esp-15.2.0_20251204",
                "esp32c5",
                "",
                str(ctx["project_dir"]),
            ]
            res = subprocess.run(cmd, env=ctx["env"], capture_output=True, text=True, cwd=ctx["root"])

            self.assertEqual(res.returncode, 0, f"Expected success for esp32c5: {res.stderr}")
            self.assertTrue(out_bin.exists())
            self.assertTrue(out_elf.exists())
            self.assertTrue(out_map.exists())
            self.assertIn("OK(esp32c5)", res.stdout)

    def test_08_defs_bzl_shizuku_firmware_binary_action_wiring(self):
        """Statically verify defs.bzl derives and passes project_dir as 6th argument and declares 3 outputs."""
        defs_content = DEFS_BZL_SRC.read_text(encoding="utf-8")

        self.assertIn("def _shizuku_firmware_binary_impl(ctx):", defs_content)
        self.assertIn('f.basename == "CMakeLists.txt"', defs_content)
        self.assertIn("project_dir = project_cmakelists[0].dirname", defs_content)
        self.assertIn("args.add(project_dir)", defs_content)
        self.assertIn("outputs = [out_bin, out_elf, out_map]", defs_content)
        self.assertIn('executable = build_sh', defs_content)


if __name__ == "__main__":
    unittest.main(verbosity=2)
