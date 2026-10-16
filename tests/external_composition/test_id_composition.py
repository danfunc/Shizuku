#!/usr/bin/env python3
"""Build-free regression checks for external composition ID definitions."""

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
from check_object_id_nm import check_nm_output  # noqa: E402


class ExternalCompositionIdTest(unittest.TestCase):
    def test_fixture_uses_relative_override_and_reuses_one_object_in_two_firmwares(self):
        module_text = (FIXTURE / "MODULE.bazel").read_text(encoding="utf-8")
        build_text = (FIXTURE / "BUILD.bazel").read_text(encoding="utf-8")
        self.assertIn('path = "../.."', module_text)
        self.assertNotIn("/Users/", module_text)
        self.assertEqual(build_text.count('name = "shared_sensor"'), 1)
        self.assertEqual(build_text.count('name = "consumer_'), 2)
        self.assertEqual(build_text.count('":shared_sensor"'), 2)
        self.assertIn('":status_sketch"', build_text)
        self.assertIn('":status_sketch_second"', build_text)

    def generate_definitions(self, names_file, output_path):
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/gen_object_ids.py"),
                "--source-template", str(ROOT / "tools/object_ids.c.in"),
                "--source-out", str(output_path),
                "--limit", "64",
                str(FIXTURE / names_file),
            ],
            check=True,
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        return output_path.read_text(encoding="utf-8")

    def test_each_external_firmware_defines_its_ids_once(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            alpha = self.generate_definitions("consumer_alpha.names", pathlib.Path(temp_dir) / "alpha.c")
            beta = self.generate_definitions("consumer_beta.names", pathlib.Path(temp_dir) / "beta.c")

        symbol_re = re.compile(r"^const uintptr_t (shizuku_object_id_[A-Za-z_][A-Za-z0-9_]*) =", re.MULTILINE)
        alpha_symbols = symbol_re.findall(alpha)
        beta_symbols = symbol_re.findall(beta)
        self.assertEqual(alpha_symbols.count("shizuku_object_id_shared_sensor"), 1)
        self.assertEqual(alpha_symbols.count("shizuku_object_id_status_sketch"), 1)
        self.assertEqual(beta_symbols.count("shizuku_object_id_shared_sensor"), 1)
        self.assertEqual(beta_symbols.count("shizuku_object_id_status_sketch_second"), 1)
        self.assertEqual(len(alpha_symbols), len(set(alpha_symbols)))
        self.assertEqual(len(beta_symbols), len(set(beta_symbols)))
        # The shared object has the same ID in this fixture because both lists
        # sort to the same first name; this validates a stable C ABI reference.
        self.assertIn("shizuku_object_id_shared_sensor = 2", alpha)
        self.assertIn("shizuku_object_id_shared_sensor = 2", beta)

    def test_archive_checker_accepts_same_member_basename_with_distinct_ids(self):
        nm_output = """libalpha.a(object_ids.c.o): 00000000 D shizuku_object_id_shared_sensor
libbeta.a(object_ids.c.o): 00000008 D shizuku_object_id_status_sketch
"""
        errors, definitions = check_nm_output(
            nm_output,
            ["shizuku_object_id_shared_sensor", "shizuku_object_id_status_sketch"],
        )
        self.assertEqual(errors, [])
        self.assertEqual(definitions["shizuku_object_id_shared_sensor"], 1)
        self.assertEqual(definitions["shizuku_object_id_status_sketch"], 1)

    def test_archive_checker_rejects_duplicate_actual_id_definition(self):
        nm_output = """libconsumer.a(object_ids.c.o): 00000000 D shizuku_object_id_shared_sensor
libconsumer.a(legacy_ids.c.o): 00000000 D shizuku_object_id_shared_sensor
"""
        errors, definitions = check_nm_output(
            nm_output,
            ["shizuku_object_id_shared_sensor"],
        )
        self.assertEqual(definitions["shizuku_object_id_shared_sensor"], 2)
        self.assertIn(
            "expected one definition of shizuku_object_id_shared_sensor, got 2",
            errors,
        )

    def test_archive_checker_does_not_count_undefined_reference(self):
        nm_output = "libconsumer.a(user.o):                  U shizuku_object_id_shared_sensor\n"
        errors, definitions = check_nm_output(
            nm_output,
            ["shizuku_object_id_shared_sensor"],
        )
        self.assertEqual(definitions, {})
        self.assertEqual(errors, ["missing definition: shizuku_object_id_shared_sensor"])

    def test_archive_checker_does_not_count_local_definition(self):
        nm_output = "libconsumer.a(local.o): 00000000 r shizuku_object_id_shared_sensor\n"
        errors, definitions = check_nm_output(
            nm_output,
            ["shizuku_object_id_shared_sensor"],
        )
        self.assertEqual(definitions, {})
        self.assertEqual(errors, ["missing definition: shizuku_object_id_shared_sensor"])

    def test_archive_checker_rejects_unexpected_global_id(self):
        nm_output = "libconsumer.a(ids.o): 00000000 R shizuku_object_id_unexpected\n"
        errors, definitions = check_nm_output(nm_output, ["shizuku_object_id_shared_sensor"])
        self.assertEqual(definitions["shizuku_object_id_unexpected"], 1)
        self.assertIn("missing definition: shizuku_object_id_shared_sensor", errors)
        self.assertIn("unexpected definition: shizuku_object_id_unexpected", errors)


if __name__ == "__main__":
    unittest.main()
