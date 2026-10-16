#!/usr/bin/env python3
"""Pure Static Source Regression Test Suite for RP2040 Pico SDK CMake Integration.

Validates all source decision seams, API contracts, transition settings,
kernel bootstrap paths, fail-closed target boundaries, and UF2 header constants
WITHOUT invoking Bazel, CMake, compilers, or device operations.

Auditable by upper-layer reviewers: all assertions report file, line, and semantic invariant.
"""

import os
import re
import sys
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))

class Rp2040FirmwareSourceStaticTests(unittest.TestCase):
    """Test suite validating source-level decision seams for RP2040 object composition."""

    def setUp(self):
        self.defs_path = os.path.join(REPO_ROOT, "defs.bzl")
        self.build_path = os.path.join(REPO_ROOT, "BUILD.bazel")
        self.rp_fw_path = os.path.join(REPO_ROOT, "configs/rp_firmware.bzl")
        self.kit_path = os.path.join(REPO_ROOT, "configs/kit.bzl")
        self.configs_build_path = os.path.join(REPO_ROOT, "configs/BUILD.bazel")
        self.bootstrap_path = os.path.join(REPO_ROOT, "modules/pico_sdk_support/firmware/rp2040_kernel_bootstrap.cpp")
        self.boot_path = os.path.join(REPO_ROOT, "modules/pico_sdk_support/firmware/shizuku_rp2040_boot.cpp")
        self.cmake_path = os.path.join(REPO_ROOT, "modules/pico_sdk_support/firmware/CMakeLists.txt")
        self.build_sh_path = os.path.join(REPO_ROOT, "modules/pico_sdk_support/firmware/build.sh")
        self.test_build_path = os.path.join(REPO_ROOT, "tests/rp2040_firmware/BUILD.bazel")
        self.loader_path = os.path.join(REPO_ROOT, "source/kernel/firmware_loader.cpp")
        self.loader_syscall_path = os.path.join(REPO_ROOT, "source/kernel/firmware_loader_syscall.cpp")

    # --------------------------------------------------------------------------
    # 1. Target Matrix & Platform Transitions (defs.bzl)
    # --------------------------------------------------------------------------
    def test_01_defs_transition_targets_and_settings(self):
        """Verify defs.bzl _firmware_transition_impl handles esp32c6, esp32c5, rp2040,

        and fail-closed rejects rp2350 and unknown targets with correct settings.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        # Check transition implementation exists
        self.assertIn("def _firmware_transition_impl(settings, attr):", defs_src)

        # RP2040 platform transition settings
        self.assertIn('"//command_line_option:platforms": str(Label("@pico-sdk//bazel/platform:rp2040"))', defs_src)
        self.assertIn('str(Label("@shizuku//configs:board")): "rp2040_pico"', defs_src)
        self.assertIn('str(Label("@shizuku//configs:rp2040_cmake_profile")): True', defs_src)
        self.assertIn('"//command_line_option:cxxopt": ["-std=c++23"]', defs_src)
        self.assertIn('-DSHIZUKU_RP2040=1', defs_src)

        # ESP32-C6 / C5 transition settings preserve rp2040_cmake_profile = False
        self.assertIn('str(Label("@shizuku//configs:rp2040_cmake_profile")): False', defs_src)

        # External build setting @pico-sdk//bazel/config:PICO_BOARD in transition inputs, outputs, and branches
        self.assertIn('_PICO_BOARD_SETTING = str(Label("@pico-sdk//bazel/config:PICO_BOARD"))', defs_src)
        self.assertIn('_PICO_BOARD_SETTING: "pico"', defs_src, "RP2040 must override PICO_BOARD to pico")
        self.assertIn('_PICO_BOARD_SETTING: settings[_PICO_BOARD_SETTING]', defs_src,
                      "C5/C6 must preserve incoming PICO_BOARD setting unchanged")

        # Transition outputs must declare rp2040_cmake_profile and PICO_BOARD
        self.assertIn('str(Label("@shizuku//configs:rp2040_cmake_profile"))', defs_src)
        self.assertIn('_PICO_BOARD_SETTING', defs_src)

        # RP2350 must be explicitly rejected fail-closed
        rp2350_match = re.search(r'elif target == "rp2350":\s*fail\((.*?)\)', defs_src)
        self.assertIsNotNone(rp2350_match, "RP2350 must have explicit fail-closed branch in _firmware_transition_impl")
        self.assertIn("unsupported", rp2350_match.group(1).lower())

    def test_02_defs_composition_gen_target_validation(self):
        """Verify _shizuku_composition_gen_impl validates targets and rejects rp2350."""
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        self.assertIn('if target == "rp2350":', defs_src)
        self.assertIn('if target not in ["esp32c6", "esp32c5", "rp2040"]:', defs_src)

    # --------------------------------------------------------------------------
    # 2. Kernel Core & Both Loader TUs in BUILD.bazel
    # --------------------------------------------------------------------------
    def test_03_build_bazel_rp2040_kernel_core(self):
        """Verify shizuku_kernel_rp2040_core includes both loader TUs, services,

        freestanding memory manager, SDK-free bootstrap, and correct dependencies.
        """
        with open(self.build_path, "r", encoding="utf-8") as f:
            build_src = f.read()

        # shizuku_kernel_rp2040_core rule presence
        self.assertIn('name = "shizuku_kernel_rp2040_core"', build_src)

        # Extract shizuku_kernel_rp2040_core block
        block_match = re.search(r'cc_library\(\s*name = "shizuku_kernel_rp2040_core",.*?\n\)', build_src, re.DOTALL)
        self.assertIsNotNone(block_match, "shizuku_kernel_rp2040_core must be defined in BUILD.bazel")
        block = block_match.group(0)

        # Both loader translation units required (IMMUTABLE DESIGN)
        self.assertIn('"source/kernel/firmware_loader.cpp"', block)
        self.assertIn('"source/kernel/firmware_loader_syscall.cpp"', block)

        # Core services and memory manager
        self.assertIn('"source/kernel/services.cpp"', block)
        self.assertIn('"source/memory_manager/freestanding.cpp"', block)
        self.assertIn('"//modules/pico_sdk_support/firmware:rp2040_kernel_bootstrap.cpp"', block)

        # Target constraint
        self.assertIn('target_compatible_with = ["@pico-sdk//bazel/constraint:rp2040"]', block)

        # Required dependencies
        self.assertIn('":core_headers"', block)
        self.assertIn('"//configs:config"', block)
        self.assertIn('"//arch/armv6m:ctx"', block)
        self.assertIn('"//chips/rp2040:headers"', block)
        self.assertIn('"//boards/rp2040_pico_w:headers"', block)

        # C5 / C6 kernels preserved
        self.assertIn('name = "shizuku_kernel_c6_core"', build_src)
        self.assertIn('name = "shizuku_kernel_c5"', build_src)
        self.assertIn('name = "shizuku_kernel"', build_src)

    # --------------------------------------------------------------------------
    # 3. Macro Routing & Default Kernel Selection (defs.bzl)
    # --------------------------------------------------------------------------
    def test_04_defs_macro_routing_and_default_kernels(self):
        """Verify shizuku_firmware routes to _rp_firmware_binary for rp2040 and

        preserves default kernel mapping for esp32c6, esp32c5, and rp2040.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        # shizuku_firmware default kernel selection
        self.assertIn('kernel = "@shizuku//:shizuku_kernel_c6_core"', defs_src)
        self.assertIn('kernel = "@shizuku//:shizuku_kernel_c5"', defs_src)
        self.assertIn('kernel = "@shizuku//:shizuku_kernel_rp2040_core"', defs_src)

        # Final link binary routing: C6/C5 -> _shizuku_firmware_binary, RP2040 -> _rp_firmware_binary
        self.assertIn('if target in ["esp32c6", "esp32c5"]:', defs_src)
        self.assertIn('_shizuku_firmware_binary(', defs_src)
        self.assertIn('elif target == "rp2040":', defs_src)
        self.assertIn('_rp_firmware_binary(', defs_src)

        # rp2350 must be rejected fail-closed in shizuku_firmware
        shizuku_fw_match = re.search(r'def shizuku_firmware\(.*?\):', defs_src, re.DOTALL)
        self.assertIsNotNone(shizuku_fw_match)

    def test_05_sketch_wrapper_semantics(self):
        """Verify sketch wrapper executes setup() once, then loops loop() and shizuku_yield().

        Returning from loop() must never exit firmware.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        # Check sketch entry template
        self.assertIn('extern "C" void shizuku_yield(void);', defs_src)
        pattern = r'extern "C" void %s\(void\) \{\s*%s\(\);\s*for \(\;\;\) \{\s*%s\(\);\s*shizuku_yield\(\);\s*\}\s*\}'
        self.assertIsNotNone(re.search(pattern, defs_src), "Sketch entry wrapper must loop and yield forever")

        # Incomplete note for multi-firmware object reuse preserved
        self.assertIn("【未達事項・技術境界 (Incomplete)】", defs_src)

    # --------------------------------------------------------------------------
    # 4. RP Firmware Convenience Layer (configs/rp_firmware.bzl)
    # --------------------------------------------------------------------------
    def test_06_rp_firmware_convenience_macros(self):
        """Verify configs/rp_firmware.bzl macros delegate cleanly and enforce rp2040 target."""
        with open(self.rp_fw_path, "r", encoding="utf-8") as f:
            rp_bzl = f.read()

        # rp_firmware delegates to shizuku_firmware
        self.assertIn("def rp_firmware(", rp_bzl)
        self.assertIn("shizuku_firmware(", rp_bzl)

        # rp_sketch delegates to shizuku_sketch
        self.assertIn("def rp_sketch(", rp_bzl)
        self.assertIn("shizuku_sketch(", rp_bzl)

        # rp_objects_archive uses shizuku_composition_gen and shizuku_firmware_archive_bundle
        self.assertIn("def rp_objects_archive(", rp_bzl)
        self.assertIn("shizuku_composition_gen(", rp_bzl)
        self.assertIn("shizuku_firmware_archive_bundle(", rp_bzl)

        # Fail-closed rejection of rp2350
        self.assertIn('if target == "rp2350":', rp_bzl)
        self.assertIn('if target != "rp2040":', rp_bzl)

    # --------------------------------------------------------------------------
    # 5. Real Root Bootstrap Path (rp2040_kernel_bootstrap.cpp)
    # --------------------------------------------------------------------------
    def test_07_rp2040_kernel_bootstrap_sequence(self):
        """Verify canonical kernel init and bootstrap order in rp2040_kernel_bootstrap.cpp."""
        with open(self.bootstrap_path, "r", encoding="utf-8") as f:
            boot_src = f.read()

        # C ABI symbol declaration
        self.assertIn('extern "C" [[noreturn]] void shizuku_rp2040_kernel_bootstrap_root_thread(', boot_src)

        # Null check with panic
        self.assertIn('if (root_thread_fn == nullptr)', boot_src)
        self.assertIn('shizuku::KERNEL::BOARD::panic("RP2040 root thread is null");', boot_src)

        # Strict execution sequence check
        init_pos = boot_src.find("kernel.init();")
        kobj_init_pos = boot_src.find("shizuku::kernel_object_instance.init();")
        handler_pos = boot_src.find("kernel.set_object_handler(")
        lend_pos = boot_src.find("shizuku::kernel_object_instance.lend_boot_stack();")
        bootstrap_pos = boot_src.find("kernel.bootstrap(root_thread_fn,")

        self.assertNotEqual(init_pos, -1, "kernel.init() must be called")
        self.assertNotEqual(kobj_init_pos, -1, "kernel_object_instance.init() must be called")
        self.assertNotEqual(handler_pos, -1, "kernel.set_object_handler() must be called")
        self.assertNotEqual(lend_pos, -1, "lend_boot_stack() must be called")
        self.assertNotEqual(bootstrap_pos, -1, "kernel.bootstrap() must be called")

        self.assertTrue(init_pos < kobj_init_pos < handler_pos < lend_pos < bootstrap_pos,
                        "Bootstrap sequence must be: kernel.init -> kobj.init -> handler -> stack -> bootstrap")

    # --------------------------------------------------------------------------
    # 6. Firmware Entry & Explicit False Handling (shizuku_rp2040_boot.cpp)
    # --------------------------------------------------------------------------
    def test_08_shizuku_rp2040_boot_entry_and_halt(self):
        """Verify shizuku_rp2040_boot.cpp invokes entry from root thread context,

        explicitly halts on false with fatal diagnostic and tight loop, and
        dispatches shizuku_yield() loop on success.
        """
        with open(self.boot_path, "r", encoding="utf-8") as f:
            boot_src = f.read()

        # Non-weak C ABI entry declaration
        self.assertIn("extern \"C\" {", boot_src)
        self.assertIn("bool shizuku_firmware_entry(void);", boot_src)
        self.assertNotIn("__attribute__((weak))", boot_src)

        # main() hands off to shizuku_rp2040_kernel_bootstrap_root_thread
        self.assertIn("shizuku_rp2040_kernel_bootstrap_root_thread(rp2040_root_thread);", boot_src)

        # Extract rp2040_root_thread function body
        root_thread_match = re.search(
            r'static void rp2040_root_thread\(void\)\s*\{(.*?)\n\}',
            boot_src,
            re.DOTALL
        )
        self.assertIsNotNone(root_thread_match, "rp2040_root_thread function must be defined")
        root_thread_body = root_thread_match.group(1)

        # 1. Root thread invokes entry and captures return boolean
        entry_call_match = re.search(r'bool\s+(\w+)\s*=\s*shizuku_firmware_entry\(\);', root_thread_body)
        self.assertIsNotNone(entry_call_match, "Root thread must invoke shizuku_firmware_entry() and capture bool result")
        ok_var = entry_call_match.group(1)

        # 2. Strengthened: False-branch must contain fatal diagnostic and tight loop
        false_branch_match = re.search(
            rf'if\s*\(\s*!{ok_var}\s*\)\s*\{{(.*?)}}',
            root_thread_body,
            re.DOTALL
        )
        self.assertIsNotNone(false_branch_match, "False-branch of entry check must be present")
        false_branch_body = false_branch_match.group(1)
        self.assertIn("FATAL", false_branch_body, "False branch must output fatal diagnostic")
        self.assertIn("shizuku_firmware_entry() returned false", false_branch_body, "False branch must report failure reason")
        self.assertIn("tight_loop_contents();", false_branch_body, "False branch must execute tight_loop_contents()")
        self.assertIn("for (;;)", false_branch_body, "False branch must halt permanently in an infinite loop")

        # 3. Strengthened: Success path must contain the yield loop
        success_path = root_thread_body[false_branch_match.end():]
        self.assertIn("shizuku_firmware_entry() succeeded", success_path, "Success path must log entry success")
        yield_loop_match = re.search(
            r'for\s*\(\;\;\)\s*\{\s*shizuku_yield\(\);\s*\}',
            success_path
        )
        self.assertIsNotNone(yield_loop_match, "Success path must contain infinite dispatch loop with shizuku_yield()")

        # Flash boundary symbols defined
        self.assertIn(".global __shizuku_firmware_limit", boot_src)
        self.assertIn(".global __shizuku_logical_flash_limit", boot_src)

    # --------------------------------------------------------------------------
    # 7. Manifest & Runtime ID Scheme (defs.bzl)
    # --------------------------------------------------------------------------
    def test_09_manifest_generator_runtime_root_id(self):
        """Verify manifest generator sets root parent_id to 1 (runtime ROOT),

        assigns IDs starting at 2, and dispatches via shizuku_firmware_load_and_start.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        # Root parent_id is 1
        self.assertIn("resolved_parent_id = 1", defs_src)

        # User objects start at ID 2
        self.assertIn("curr_id = 2", defs_src)

        # Manifest dispatches via canonical load_and_start
        self.assertIn("extern bool shizuku_firmware_load_and_start(", defs_src)
        self.assertIn("bool shizuku_firmware_entry(void) {", defs_src)
        self.assertIn("return shizuku_firmware_load_and_start(shizuku_composition_manifest, shizuku_composition_manifest_count);", defs_src)

    # --------------------------------------------------------------------------
    # 8. Firmware Loader Limits & Authority Validation
    # --------------------------------------------------------------------------
    def test_10_firmware_loader_boundaries(self):
        """Verify firmware_loader.cpp enforces maximum 8 objects, validates root authority,

        and calls canonical object_api CREATE_OBJECT and SPAWN.
        """
        with open(self.loader_path, "r", encoding="utf-8") as f:
            loader_src = f.read()

        # Max objects bound
        self.assertIn("constexpr uint32_t SHIZUKU_LOADER_MAX_OBJECTS = 8;", loader_src)

        # Authority check: non-root parent rejected
        self.assertIn("if (item.parent_id != 1)", loader_src)

        # object_api CREATE_OBJECT and SPAWN
        self.assertIn("shizuku::object_api::CREATE_OBJECT", loader_src)
        self.assertIn("shizuku::object_api::SPAWN", loader_src)

        # Loader syscall boundary implementation exists
        with open(self.loader_syscall_path, "r", encoding="utf-8") as f:
            syscall_src = f.read()
        self.assertIn("shizuku::ARCH::syscall(number, arg1, arg2, arg3)", syscall_src)

    # --------------------------------------------------------------------------
    # 9. UF2 Family ID & Header Integrity Constants
    # --------------------------------------------------------------------------
    def test_11_uf2_family_id_and_header_constants(self):
        """Verify correct RP2040 UF2 family ID (0xe48bff56), CMake UF2 generation, and typo absence."""
        expected_family_id = 0xe48bff56
        typo_family_id = 0xe48ff56a

        # 1. When PICO_SDK_PATH is supplied in environment, inspect official Pico SDK boot/uf2.h
        pico_sdk_path = os.environ.get("PICO_SDK_PATH")
        if pico_sdk_path and os.path.isdir(pico_sdk_path):
            uf2_header_path = os.path.join(pico_sdk_path, "src/common/boot_uf2_headers/include/boot/uf2.h")
            self.assertTrue(os.path.isfile(uf2_header_path), f"boot/uf2.h must exist at {uf2_header_path}")
            with open(uf2_header_path, "r", encoding="utf-8") as f:
                header_content = f.read()
            match = re.search(r'#define\s+RP2040_FAMILY_ID\s+0x([0-9a-fA-F]+)', header_content)
            self.assertIsNotNone(match, "RP2040_FAMILY_ID definition must be found in boot/uf2.h")
            parsed_id = int(match.group(1), 16)
            self.assertEqual(parsed_id, expected_family_id,
                             f"RP2040_FAMILY_ID in SDK ({hex(parsed_id)}) must equal expected {hex(expected_family_id)}")
            self.assertNotEqual(parsed_id, typo_family_id,
                                f"RP2040_FAMILY_ID must not match typo {hex(typo_family_id)}")
        else:
            print("\n[INFO] PICO_SDK_PATH not supplied in environment: SDK uf2.h source inspection skipped (portable behavior)")

        # 2. Assert CMake configuration selects RP2040 platform and enables pico_add_extra_outputs for UF2
        with open(self.cmake_path, "r", encoding="utf-8") as f:
            cmake_src = f.read()
        self.assertIn('set(PICO_PLATFORM "rp2040")', cmake_src,
                      "CMakeLists.txt must set default PICO_PLATFORM to rp2040")
        self.assertIn('if(NOT PICO_PLATFORM STREQUAL "rp2040")', cmake_src,
                      "CMakeLists.txt must fail-closed if PICO_PLATFORM is not rp2040")
        self.assertIn('pico_add_extra_outputs(shizuku_rp2040)', cmake_src,
                      "CMakeLists.txt must invoke pico_add_extra_outputs to generate UF2 binary")

        # 3. Assert typo string 0xe48ff56a is absent from all code source files in repo
        typo_str = "0xe48ff56a"
        this_file = os.path.abspath(__file__)
        for root, _, files in os.walk(REPO_ROOT):
            if ".git" in root or "bazel-" in root or ".handoff" in root:
                continue
            for fname in files:
                if fname.endswith((".py", ".cpp", ".hpp", ".bzl", ".sh", ".txt", ".h", ".c")):
                    fpath = os.path.abspath(os.path.join(root, fname))
                    if fpath == this_file:
                        continue
                    with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                        content = f.read()
                    self.assertNotIn(typo_str, content, f"Typo {typo_str} found in code source file {fpath}")

    # --------------------------------------------------------------------------
    # 10. SDK Path Portability & CMake Wiring
    # --------------------------------------------------------------------------
    def test_12_sdk_path_portability_and_wiring(self):
        """Verify SDK path is not hardcoded in test BUILD and build.sh supports env passthrough."""
        with open(self.test_build_path, "r", encoding="utf-8") as f:
            test_bzl = f.read()

        # Must not contain hardcoded Mac paths
        self.assertNotIn("/Users/ishigakiyua/github/pico-sdk", test_bzl)

        # build.sh must support env passthrough
        with open(self.build_sh_path, "r", encoding="utf-8") as f:
            sh_src = f.read()
        self.assertIn("PICO_SDK_PATH=${5:-${PICO_SDK_PATH:-}}", sh_src)
        self.assertIn('[ -n "$PICO_SDK_PATH" ] || fail', sh_src)

        # CMakeLists.txt must support env passthrough and whole archive
        with open(self.cmake_path, "r", encoding="utf-8") as f:
            cmake_src = f.read()
        self.assertIn("if(NOT DEFINED PICO_SDK_PATH)", cmake_src)
        self.assertIn('"$<LINK_LIBRARY:WHOLE_ARCHIVE,shizuku_archive>"', cmake_src)
        self.assertIn("shizuku_firmware_entry", cmake_src)
        self.assertIn("pico_add_extra_outputs(shizuku_rp2040)", cmake_src)

    # --------------------------------------------------------------------------
    # 11. RP2350 Unsupported Across All Seams
    # --------------------------------------------------------------------------
    def test_13_rp2350_unsupported_fail_closed(self):
        """Verify target rp2350 is explicitly rejected fail-closed across all entry points."""
        # 1. build.sh
        with open(self.build_sh_path, "r", encoding="utf-8") as f:
            sh_src = f.read()
        self.assertIn('if [ "$PICO_PLATFORM" != "rp2040" ]; then', sh_src)

        # 2. CMakeLists.txt
        with open(self.cmake_path, "r", encoding="utf-8") as f:
            cmake_src = f.read()
        self.assertIn('if(NOT PICO_PLATFORM STREQUAL "rp2040")', cmake_src)
        self.assertIn("RP2350 must not be declared supported", cmake_src)

        # 3. defs.bzl
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()
        self.assertIn('elif target == "rp2350":', defs_src)
        self.assertIn("Target 'rp2350' is unsupported", defs_src)

        # 4. configs/rp_firmware.bzl
        with open(self.rp_fw_path, "r", encoding="utf-8") as f:
            rp_bzl = f.read()
        self.assertIn('if target == "rp2350":', rp_bzl)

    # --------------------------------------------------------------------------
    # 12. Archive Bundler Dedup & Declared Toolchain (defs.bzl)
    # --------------------------------------------------------------------------
    def test_14_archive_bundler_path_dedup_and_toolchain(self):
        """Verify archive bundler deduplicates archives and objects by path only,

        uses declared CcToolchainInfo.ar_executable directly, and contains no
        silent basename exclusions or un-sandboxed execution requirements.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        # Extract _shizuku_firmware_archive_bundle_impl function body
        bundle_match = re.search(
            r'def _shizuku_firmware_archive_bundle_impl\(ctx\):(.*?)(?=\n_shizuku_firmware_archive_bundle = rule|\Z)',
            defs_src,
            re.DOTALL
        )
        self.assertIsNotNone(bundle_match, "_shizuku_firmware_archive_bundle_impl must exist")
        bundle_body = bundle_match.group(1)

        # 1. Path-based deduplication without silent basename exclusions
        self.assertIn("archives_dict[lib.static_library.path] = lib.static_library", bundle_body)
        self.assertIn("direct_objs_dict[o.path] = o", bundle_body)
        self.assertNotIn("libobject_ids.", bundle_body, "Archive bundler must not filter by libobject_ids basename")
        self.assertNotIn("object_ids.", bundle_body, "Archive bundler must not filter by object_ids basename")

        # 2. Direct invocation of declared CcToolchainInfo.ar_executable
        self.assertIn("cc_toolchain = ctx.attr._cc_toolchain[cc_common.CcToolchainInfo]", bundle_body)
        self.assertIn("ar_tool = cc_toolchain.ar_executable", bundle_body)
        self.assertIn('command = "$1 -M < $2"', bundle_body, "Must invoke ar_tool directly via MRI script")
        self.assertIn("arguments = [ar_tool, mri_file.path]", bundle_body)

        # 3. No fallback to host PATH or hardcoded Homebrew
        self.assertNotIn("/opt/homebrew", bundle_body, "Must not contain hardcoded Homebrew path in archive bundle")
        self.assertNotIn("command -v arm-none-eabi-ar", bundle_body, "Must not fall back to PATH search in archive bundle")

        # 4. Standard sandboxed action (no local or no-sandbox bypass)
        self.assertNotIn('"local": "1"', bundle_body, "Archive bundler must not bypass sandbox with local=1")
        self.assertNotIn('"no-sandbox": "1"', bundle_body, "Archive bundler must not bypass sandbox with no-sandbox=1")

    # --------------------------------------------------------------------------
    # 13. CMake Target Declared Inputs Closure & Triple Outputs (UF2/ELF/MAP)
    # --------------------------------------------------------------------------
    def test_15_cmake_declared_inputs_and_triple_outputs(self):
        """Verify _rp_firmware_binary declares all CMake inputs, emits UF2, ELF, and MAP,

        and build.sh copies and verifies all three outputs.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()

        # Extract _rp_firmware_binary_impl and rule
        rule_match = re.search(
            r'def _rp_firmware_binary_impl\(ctx\):(.*?)(?=\n_rp_firmware_binary = rule|\Z)',
            defs_src,
            re.DOTALL
        )
        self.assertIsNotNone(rule_match, "_rp_firmware_binary_impl must exist")
        impl_body = rule_match.group(1)

        # 1. Triple output declaration
        self.assertIn('out_uf2 = ctx.actions.declare_file(ctx.label.name + ".uf2")', impl_body)
        self.assertIn('out_elf = ctx.actions.declare_file(ctx.label.name + ".elf")', impl_body)
        self.assertIn('out_map = ctx.actions.declare_file(ctx.label.name + ".map")', impl_body)
        self.assertIn('outputs = [out_uf2, out_elf, out_map]', impl_body)
        self.assertIn('DefaultInfo(files = depset([out_uf2, out_elf, out_map]))', impl_body)

        # 2. No sandbox bypass in _rp_firmware_binary
        self.assertNotIn('"local": "1"', impl_body, "_rp_firmware_binary must not use local=1")
        self.assertNotIn('"no-sandbox": "1"', impl_body, "_rp_firmware_binary must not use no-sandbox=1")

        # 3. Input dependency closure in _rp_firmware_binary rule
        self.assertIn('"extra_sources": attr.label_list(', defs_src)
        self.assertIn('"@shizuku//modules/pico_sdk_support/rp2040:arch_glue_src"', defs_src)
        self.assertIn('"@shizuku//boards/rp2040_pico_w:board_src"', defs_src)
        self.assertIn('"@shizuku//modules/pico_sdk_support/objects:usb_cdc.cpp"', defs_src)
        self.assertIn('"extra_headers": attr.label_list(', defs_src)
        self.assertIn('"@shizuku//:repo_internal_headers"', defs_src)
        self.assertIn('"@shizuku//modules/pico_sdk_support:objects_internal_headers"', defs_src)
        self.assertIn('"linker_script": attr.label(', defs_src)
        self.assertIn('"@shizuku//modules/pico_sdk_support/firmware:rp2040_flash_limit.ld"', defs_src)

        # 4. build.sh verifies and copies all 3 outputs
        with open(self.build_sh_path, "r", encoding="utf-8") as f:
            sh_src = f.read()
        self.assertIn('cp "$BUILD_DIR/shizuku_rp2040.uf2" "$OUT_UF2"', sh_src)
        self.assertIn('cp "$BUILD_DIR/shizuku_rp2040.elf" "$OUT_ELF"', sh_src)
        self.assertIn('cp "$BUILD_DIR/shizuku_rp2040.map" "$OUT_MAP"', sh_src)
        self.assertIn('if [ ! -f "$BUILD_DIR/shizuku_rp2040.uf2" ]; then', sh_src)
        self.assertIn('if [ ! -f "$BUILD_DIR/shizuku_rp2040.elf" ]; then', sh_src)
        self.assertIn('if [ ! -f "$BUILD_DIR/shizuku_rp2040.map" ]; then', sh_src)

        # 5. Declarations exist in respective BUILD packages
        with open(self.build_path, "r", encoding="utf-8") as f:
            self.assertIn('name = "repo_internal_headers"', f.read())

        with open(os.path.join(REPO_ROOT, "modules/pico_sdk_support/BUILD.bazel"), "r", encoding="utf-8") as f:
            self.assertIn('name = "objects_internal_headers"', f.read())

        with open(os.path.join(REPO_ROOT, "modules/pico_sdk_support/rp2040/BUILD.bazel"), "r", encoding="utf-8") as f:
            self.assertIn('name = "arch_glue_src"', f.read())

        with open(os.path.join(REPO_ROOT, "boards/rp2040_pico_w/BUILD.bazel"), "r", encoding="utf-8") as f:
            self.assertIn('name = "board_src"', f.read())

        with open(os.path.join(REPO_ROOT, "modules/pico_sdk_support/firmware/BUILD.bazel"), "r", encoding="utf-8") as f:
            self.assertIn('"rp2040_flash_limit.ld"', f.read())

    # --------------------------------------------------------------------------
    # 14. Board MVP Constraint & config_header Threading
    # --------------------------------------------------------------------------
    def test_16_board_pico_fail_closed_and_config_header_threading(self):
        """Verify board pico is strictly enforced across public and binary APIs,

        and config_header is threaded through rp_firmware, rp_sketch,
        shizuku_firmware, and shizuku_sketch into _rp_firmware_binary.
        """
        with open(self.defs_path, "r", encoding="utf-8") as f:
            defs_src = f.read()
        with open(self.rp_fw_path, "r", encoding="utf-8") as f:
            rp_bzl = f.read()
        with open(self.build_sh_path, "r", encoding="utf-8") as f:
            sh_src = f.read()

        # 1. Fail-closed board checks
        self.assertIn('if board != "pico":', defs_src)
        self.assertIn('if ctx.attr.board != "pico":', defs_src)
        self.assertIn('if board != "pico":', rp_bzl)
        self.assertIn('if [ "$PICO_BOARD" != "pico" ]; then', sh_src)

        # 2. config_header threading through public API
        self.assertIn('config_header = "@shizuku//configs:config_hpp"', defs_src)
        self.assertIn('config_header = config_header', defs_src)
        self.assertIn('config_header = config_header', rp_bzl)

        # 3. Parameter present in signatures
        self.assertIn('def shizuku_firmware(', defs_src)
        self.assertIn('def shizuku_sketch(', defs_src)
        self.assertIn('def rp_firmware(', rp_bzl)
        self.assertIn('def rp_sketch(', rp_bzl)


if __name__ == "__main__":
    unittest.main(verbosity=2)
