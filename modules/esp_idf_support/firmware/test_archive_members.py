#!/usr/bin/env python3
"""Host fixture for duplicate archive member names and per-member symbol checks."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from check_archive_members import check_archive_members, strong_symbols


def command(args, *, cwd=None):
    return subprocess.run(args, cwd=cwd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class DuplicateArchiveMembersTest(unittest.TestCase):
    def test_both_same_named_members_pass_and_missing_member_fails(self):
        with tempfile.TemporaryDirectory(prefix="shizuku-archive-fixture-") as temp:
            root = Path(temp)
            first, second = root / "first", root / "second"
            first.mkdir()
            second.mkdir()
            (first / "init.c").write_text("int member_one(void) { return 1; }\n")
            (second / "init.c").write_text("int member_two(void) { return 2; }\n")
            (root / "other.c").write_text("int member_three(void) { return 3; }\n")
            (root / "template.cpp").write_text("""namespace shizuku {
struct Arch {};
struct Board {};
template <class A, class B> struct kernel { static int dispatch(); };
template <> int kernel<Arch, Board>::dispatch() { return 4; }
extern "C" int template_entry() { return kernel<Arch, Board>::dispatch(); }
}
""")
            command(["cc", "-c", "init.c", "-o", "init.o"], cwd=first)
            command(["cc", "-c", "init.c", "-o", "init.o"], cwd=second)
            command(["cc", "-c", str(root / "other.c"), "-o", str(root / "other.o")])
            command(["c++", "-c", str(root / "template.cpp"), "-o", str(root / "template.o")])
            demangled = command(["nm", "-C", "-g", "--defined-only", str(root / "template.o")]).stdout
            self.assertIn("kernel<shizuku::Arch, shizuku::Board>::dispatch()", demangled)
            archive = root / "libduplicate.a"
            command(["ar", "qc", str(archive), "init.o"], cwd=first)
            command(["ar", "q", str(archive), "init.o"], cwd=second)
            command(["ar", "q", str(archive), str(root / "other.o")])
            command(["ar", "q", str(archive), str(root / "template.o")])
            command(["ranlib", str(archive)])
            archive_members = command(["ar", "t", str(archive)]).stdout.splitlines()
            self.assertEqual(archive_members.count("init.o"), 2)
            self.assertEqual(archive_members.count("other.o"), 1)
            self.assertEqual(archive_members.count("template.o"), 1)

            both_main = root / "both.c"
            both_main.write_text("int member_one(void); int member_two(void); int member_three(void); int template_entry(void); int main(void) { return member_one() + member_two() + member_three() + template_entry(); }\n")
            both_elf, both_map = root / "both.elf", root / "both.map"
            map_arg = f"-Wl,-map,{both_map}" if sys.platform == "darwin" else f"-Wl,-Map={both_map}"
            command(["cc", str(both_main), str(first / "init.o"), str(second / "init.o"), str(root / "other.o"), str(root / "template.o"), map_arg, "-o", str(both_elf)])
            members, missing = check_archive_members(archive, both_elf, both_map, "llvm-ar", "nm")
            self.assertEqual(len(members), 4)
            self.assertEqual(missing, [])

            one_main = root / "one.c"
            one_main.write_text("int member_one(void); int member_three(void); int template_entry(void); int main(void) { return member_one() + member_three() + template_entry(); }\n")
            one_elf, one_map = root / "one.elf", root / "one.map"
            one_map_arg = f"-Wl,-map,{one_map}" if sys.platform == "darwin" else f"-Wl,-Map={one_map}"
            command(["cc", str(one_main), str(first / "init.o"), str(root / "other.o"), str(root / "template.o"), one_map_arg, "-o", str(one_elf)])
            members, missing = check_archive_members(archive, one_elf, one_map, "llvm-ar", "nm")
            self.assertEqual(len(members), 4)
            self.assertEqual(len(missing), 1, missing)
            self.assertIn("2:init.o:no-strong-symbol-in-map-and-ELF", missing[0])

            common_source = root / "common.c"
            common_source.write_text("int common_only_symbol;\n")
            common_object = root / "common.o"
            command(["cc", "-fcommon", "-c", str(common_source), "-o", str(common_object)])
            common_nm = command(["nm", "-g", "--defined-only", str(common_object)]).stdout
            self.assertRegex(common_nm, r"\bC\s+_?common_only_symbol(?:\s|$)")
            self.assertNotIn("common_only_symbol", strong_symbols("nm", common_object))


if __name__ == "__main__":
    unittest.main(verbosity=2)
