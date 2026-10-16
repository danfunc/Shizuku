#!/usr/bin/env python3
"""Static dependency closure and symbol regression tests for ESP32-C6 firmware composition.

Validates:
1. Normal separation: boards/esp32_c6:headers depends strictly on //:core_headers and //chips/esp32_c6:headers,
   excluding //:internal_headers and :object_ids.
2. Negative fixture: mutating boards/esp32_c6 BUILD content to include //:internal_headers MUST fail extraction & validation.
3. Bounded closure trace: shizuku_kernel_c6_core dependency tree excludes legacy internal_headers / object_ids.
   (Note: Real Bazel closure resolution remains unproven without real Bazel build execution.)
4. Negative fixture: mutating dependency BUILD content within bounded trace MUST be detected and flagged.
5. Sample object alignment: sample_object.cpp/hpp uses SHIZUKU_DECLARE_OBJECT_ID(sample_obj), defines sample_obj_main
   (without volatile increment), and contains no references to obsolete test_sample_object.
6. Acceptance gate: tools/check_object_id_nm.py validates clean archive symbols (tolerating internal U references to
   sample_obj_main between members) and verifies positive entry definitions.
7. Acceptance gate failure detection: tools/check_object_id_nm.py catches duplicate IDs, missing entry definitions,
   and forbidden undefined references (shizuku_object_id_test_sample_object).
8. CLI wiring: tools/check_object_id_nm.py CLI main invocation succeeds on clean input and fails on forbidden undefined.
"""

from pathlib import Path
import re
import subprocess
import sys
import unittest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))
from check_object_id_nm import check_nm_output, check_entry_symbols, check_undefined_symbols  # noqa: E402


def extract_cc_library_deps(build_content: str, target_name: str) -> list[str]:
    """Extract deps list for a cc_library target from a BUILD.bazel file."""
    # Strip comments first so parentheses inside comments do not terminate regex prematurely
    clean_content = re.sub(r'#.*', '', build_content)
    pattern = re.compile(
        r'cc_library\s*\(\s*name\s*=\s*["\']' + re.escape(target_name) + r'["\'](.*?)\n\)',
        re.DOTALL,
    )
    match = pattern.search(clean_content)
    if not match:
        return []
    body = match.group(1)
    deps_match = re.search(r'deps\s*=\s*(\[[^\]]*\])', body, re.DOTALL)
    if not deps_match:
        return []
    raw = deps_match.group(1)
    return re.findall(r'["\']([^"\']+)["\']', raw)


def validate_c6_board_headers_deps(deps: list[str]) -> list[str]:
    """Return errors if boards/esp32_c6:headers deps leak legacy internal_headers or object_ids."""
    errors = []
    forbidden = ["//:internal_headers", ":internal_headers", "@shizuku//:internal_headers",
                 "//:object_ids", ":object_ids", "@shizuku//:object_ids"]
    for f in forbidden:
        if f in deps:
            errors.append(f"boards/esp32_c6:headers must not depend on legacy '{f}' (leaks global object IDs)")
    if "//:core_headers" not in deps and "@shizuku//:core_headers" not in deps:
        errors.append("boards/esp32_c6:headers must depend on //:core_headers for clean header separation")
    return errors


def validate_c6_kernel_core_bounded_trace(root_build: str, configs_build: str, chips_build: str, board_build: str) -> list[str]:
    """Validate bounded static dependency trace for shizuku_kernel_c6_core.

    Checks direct deps of shizuku_kernel_c6_core in root BUILD.bazel and 1st-level
    child dependencies (configs, chips/esp32_c6, boards/esp32_c6) for legacy ID leakage.
    Note: Full real Bazel closure resolution is unproven without real Bazel execution.
    """
    errors = []
    core_deps = extract_cc_library_deps(root_build, "shizuku_kernel_c6_core")
    if not core_deps:
        return ["shizuku_kernel_c6_core target not found or has no deps"]
    if ":internal_headers" in core_deps or "//:internal_headers" in core_deps:
        errors.append("shizuku_kernel_c6_core directly depends on internal_headers")
    if ":object_ids" in core_deps or "//:object_ids" in core_deps:
        errors.append("shizuku_kernel_c6_core directly depends on object_ids")

    # 1. configs
    if '"//:internal_headers"' in configs_build or '":internal_headers"' in configs_build:
        errors.append("configs/BUILD.bazel leaks internal_headers")

    # 2. chips/esp32_c6
    chip_deps = extract_cc_library_deps(chips_build, "headers")
    if "//:internal_headers" in chip_deps or ":internal_headers" in chip_deps:
        errors.append("chips/esp32_c6:headers leaks internal_headers")

    # 3. boards/esp32_c6
    board_deps = extract_cc_library_deps(board_build, "headers")
    board_errors = validate_c6_board_headers_deps(board_deps)
    errors.extend(board_errors)

    return errors


class C6ClosureRegressionTest(unittest.TestCase):
    def test_01_boards_esp32_c6_headers_excludes_internal_headers(self):
        """Positive check: boards/esp32_c6/BUILD.bazel headers depends on core_headers and not internal_headers."""
        board_build = (REPO_ROOT / "boards" / "esp32_c6" / "BUILD.bazel").read_text(encoding="utf-8")
        deps = extract_cc_library_deps(board_build, "headers")

        self.assertTrue(deps, "Failed to extract deps for boards/esp32_c6:headers")
        errors = validate_c6_board_headers_deps(deps)
        self.assertEqual(errors, [], f"Dependency separation violation: {errors}")
        self.assertIn("//:core_headers", deps)
        self.assertNotIn("//:internal_headers", deps)

    def test_02_negative_fixture_board_header_leak_detection(self):
        """Negative check: mutating boards/esp32_c6 BUILD content to include //:internal_headers MUST fail extraction & validation."""
        board_build = (REPO_ROOT / "boards" / "esp32_c6" / "BUILD.bazel").read_text(encoding="utf-8")
        self.assertIn('"//:core_headers"', board_build)
        mutated_build = board_build.replace('"//:core_headers"', '"//:internal_headers"')
        extracted_deps = extract_cc_library_deps(mutated_build, "headers")
        self.assertIn("//:internal_headers", extracted_deps)
        self.assertNotIn("//:core_headers", extracted_deps)

        errors = validate_c6_board_headers_deps(extracted_deps)
        self.assertTrue(len(errors) >= 1, "Validator failed to flag //:internal_headers leak")
        self.assertTrue(any("//:internal_headers" in e for e in errors))

    def test_03_c6_kernel_core_closure_no_legacy_object_ids(self):
        """Positive check: bounded static trace of shizuku_kernel_c6_core excludes legacy object_ids.

        Note: Real Bazel closure resolution remains unproven without real Bazel build execution.
        """
        root_build = (REPO_ROOT / "BUILD.bazel").read_text(encoding="utf-8")
        configs_build = (REPO_ROOT / "configs" / "BUILD.bazel").read_text(encoding="utf-8")
        chips_build = (REPO_ROOT / "chips" / "esp32_c6" / "BUILD.bazel").read_text(encoding="utf-8")
        board_build = (REPO_ROOT / "boards" / "esp32_c6" / "BUILD.bazel").read_text(encoding="utf-8")

        errors = validate_c6_kernel_core_bounded_trace(root_build, configs_build, chips_build, board_build)
        self.assertEqual(errors, [], f"Bounded trace detected legacy leakage: {errors}")

    def test_04_negative_fixture_closure_taint_detection(self):
        """Negative check: mutating board_build within bounded trace MUST be detected and flagged."""
        root_build = (REPO_ROOT / "BUILD.bazel").read_text(encoding="utf-8")
        configs_build = (REPO_ROOT / "configs" / "BUILD.bazel").read_text(encoding="utf-8")
        chips_build = (REPO_ROOT / "chips" / "esp32_c6" / "BUILD.bazel").read_text(encoding="utf-8")
        board_build = (REPO_ROOT / "boards" / "esp32_c6" / "BUILD.bazel").read_text(encoding="utf-8")

        # Mutate board_build to reintroduce internal_headers
        tainted_board_build = board_build.replace('"//:core_headers"', '"//:internal_headers"')
        errors = validate_c6_kernel_core_bounded_trace(root_build, configs_build, chips_build, tainted_board_build)

        self.assertTrue(len(errors) >= 1, "Bounded trace failed to detect tainted dependency")
        self.assertTrue(any("internal_headers" in e for e in errors))

    def test_05_sample_object_naming_and_c_abi_alignment(self):
        """Verify sample_object.cpp/hpp naming matches target name sample_obj and defines sample_obj_main without volatile."""
        cpp_content = (REPO_ROOT / "tests" / "firmware_composition" / "sample_object.cpp").read_text(encoding="utf-8")
        hpp_content = (REPO_ROOT / "tests" / "firmware_composition" / "sample_object.hpp").read_text(encoding="utf-8")
        build_content = (REPO_ROOT / "tests" / "firmware_composition" / "BUILD.bazel").read_text(encoding="utf-8")

        # 1. Macro declaration and namespace reference must use sample_obj
        self.assertIn("SHIZUKU_DECLARE_OBJECT_ID(sample_obj)", cpp_content)
        self.assertIn("shizuku::object_id::sample_obj", cpp_content)
        self.assertNotIn("test_sample_object", cpp_content, "Obsolete test_sample_object symbol must not be present")

        # 2. C ABI entry definitions without volatile
        self.assertIn('void sample_obj_main(void)', cpp_content)
        self.assertIn('sample_obj_main', hpp_content)
        self.assertNotIn('volatile', cpp_content, "volatile increment must not be present to avoid C++20 deprecation warnings")

        # 3. Target consistency in BUILD.bazel
        self.assertIn('name = "sample_obj"', build_content)
        self.assertIn('":sample_obj"', build_content)
        self.assertIn('"sample_obj": "root"', build_content)
        self.assertIn('"sample_obj"', build_content)

    def test_06_check_object_id_nm_clean_archive_pass(self):
        """Verify check_object_id_nm functions validate clean NM output matching sample_fw archive.

        In an archive (.a), member-to-member undefined references (e.g. manifest.o referencing U sample_obj_main)
        coexist with member definitions (sample_object.o defining T sample_obj_main). check_entry_symbols verifies
        the strong global T definition exists, while check_undefined_symbols only targets genuinely forbidden symbols.
        """
        clean_nm = """
00000000 R shizuku_object_id_root
00000004 R shizuku_object_id_count
00000008 R shizuku_object_id_sample_obj
0000000c R shizuku_object_id_sample_sketch_obj
00000010 R shizuku_object_id_sample_sketch_obj_second
         U sample_obj_main
00000020 T sample_obj_main
00000030 T sample_sketch_obj_main
00000040 T sample_sketch_obj_second_main
00000050 T shizuku_firmware_entry
"""
        expected_ids = ["shizuku_object_id_root", "shizuku_object_id_count", "shizuku_object_id_sample_obj",
                        "shizuku_object_id_sample_sketch_obj", "shizuku_object_id_sample_sketch_obj_second"]
        expected_entries = ["sample_obj_main", "sample_sketch_obj_main", "sample_sketch_obj_second_main",
                            "shizuku_firmware_entry"]
        forbidden_undefs = ["shizuku_object_id_test_sample_object"]

        id_errors, id_defs = check_nm_output(clean_nm, expected_ids)
        self.assertEqual(id_errors, [])
        self.assertEqual(len(id_defs), 5)

        entry_errors, entry_defs = check_entry_symbols(clean_nm, expected_entries)
        self.assertEqual(entry_errors, [])
        self.assertEqual(len(entry_defs), 4)

        undef_errors, _ = check_undefined_symbols(clean_nm, forbidden_undefs)
        self.assertEqual(undef_errors, [])

    def test_07_check_object_id_nm_detects_real_failure_log_discrepancies(self):
        """Verify check_object_id_nm functions catch the exact defects observed in 20261016b logs."""
        failure_nm = """
00000000 R shizuku_object_id_root
00000000 R shizuku_object_id_root
00000004 R shizuku_object_id_count
00000004 R shizuku_object_id_count
00000008 R shizuku_object_id_sample_obj
         U shizuku_object_id_test_sample_object
         U sample_obj_main
"""
        expected_ids = ["shizuku_object_id_root", "shizuku_object_id_count", "shizuku_object_id_sample_obj"]
        expected_entries = ["sample_obj_main"]
        forbidden_undefs = ["shizuku_object_id_test_sample_object"]

        # 1. Multiple definitions of root and count must be flagged
        id_errors, _ = check_nm_output(failure_nm, expected_ids)
        self.assertTrue(any("expected one definition of shizuku_object_id_root, got 2" in e for e in id_errors))
        self.assertTrue(any("expected one definition of shizuku_object_id_count, got 2" in e for e in id_errors))

        # 2. Missing entry definition (only U, no T definition) must be flagged
        entry_errors, _ = check_entry_symbols(failure_nm, expected_entries)
        self.assertIn("missing entry definition: sample_obj_main", entry_errors)

        # 3. Forbidden undefined reference must be flagged
        undef_errors, _ = check_undefined_symbols(failure_nm, forbidden_undefs)
        self.assertTrue(any("shizuku_object_id_test_sample_object" in e for e in undef_errors))

    def test_08_check_object_id_nm_cli_main(self):
        """Verify CLI main entry point of check_object_id_nm.py with stdin pipe."""
        clean_nm = "00000000 R shizuku_object_id_sample_obj\n00000010 T sample_obj_main\n"
        proc_clean = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "check_object_id_nm.py"),
                "--expected", "sample_obj",
                "--expected-entry", "sample_obj_main",
                "--reject-undefined", "shizuku_object_id_test_sample_object",
            ],
            input=clean_nm,
            text=True,
            capture_output=True,
        )
        self.assertEqual(proc_clean.returncode, 0, f"CLI clean run failed: {proc_clean.stderr}")
        self.assertIn("OK: shizuku_object_id_sample_obj defined once", proc_clean.stdout)
        self.assertIn("OK: entry sample_obj_main defined once", proc_clean.stdout)
        self.assertIn("OK: shizuku_object_id_test_sample_object not undefined", proc_clean.stdout)

        # Negative run: forbidden undefined reference
        tainted_nm = "         U shizuku_object_id_test_sample_object\n"
        proc_tainted = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "check_object_id_nm.py"),
                "--reject-undefined", "shizuku_object_id_test_sample_object",
            ],
            input=tainted_nm,
            text=True,
            capture_output=True,
        )
        self.assertEqual(proc_tainted.returncode, 1, "CLI must exit with code 1 on forbidden undefined reference")
        self.assertIn("ERROR: forbidden undefined reference found for shizuku_object_id_test_sample_object", proc_tainted.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
