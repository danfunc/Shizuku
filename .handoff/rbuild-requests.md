# Concrete rbuild Build Request for ESP32-C6 Firmware Composition

Date: 2026-10-16  
Author: Implementation Worker (Gemini conversation 934558fa)  
Caller / Reviewer: Sol (caller model `gpt-6.1-sol` unverified self-report; provenance unknown) / Opus  
Clone: `/Users/ishigakiyua/github/Shizuku-objfw`  
Branch: `feat/object-firmware-composition`  
Base HEAD: `8a6940d`  
Commit Status: Prepared before this commit; staged as a review checkpoint and remains pending only for remote build/test.

---

## 1. Tool Execution Boundary & Two-Environment Help Evidence

1. **Two-Environment Help Evidence**:
   - **Parent Environment**: The parent process executed `rbuild --help` in this clone with exit code 0, and `rbuild help job` with exit code 0, verifying the exact invocation syntax: `rbuild job <NAME>`.
   - **Worker Sandbox Environment**: Gemini's earlier result reported an attempt of `rbuild --help` with exit 127 (`zsh:1: operation not permitted: rbuild`). No tool log or exit object was furnished; this remains worker-reported and unverified, distinct from parent's confirmed help result.
   - **Build Execution Status**: **No actual firmware or test build was attempted via rbuild, and no build refusal occurred.** Do not claim a build attempt occurred or halt on the unverified help claim. Remote build execution remains pending for Opus via rbuild.

2. **Recovery Record**:
   - Implementation Gemini job `afb5bc6f-8d52-4279-80e6-7614df3e4995` finished partial, requested Flash High / actual model unknown, conversation `2aaec299-ff7e-4195-acd6-021396edd16e`.
   - Earlier C6 job `5601432a-9157-4765-a3f0-4601ec72d51b` was done but only reported that a local Bazel test had started; prohibited and not accepted as evidence.
   - RP job `e8cc612f-b879-4177-8ccb-0e6d3135c3f1` was running when killed; kill tool returned explicit 'stopped'; it has no accepted result.
   - Followup hub attempts `096c2930-6329-492a-af74-934d676c60c3` and `4465eac2-53e7-412c-a3bc-eb483fff2c74` failed immediately because hub was unavailable; no source changes resulted from those attempts.
   - Correction job `29c151c9-d26a-4714-a6d7-392c03002dba` was stopped after 5 seconds before substantive work.

---

## 2. Current Acceptance Matrix & Technical Boundaries (Root-Only Interim)

### Current Acceptance Matrix

| Item | State | Evidence / Remaining Gate |
|---|---|---|
| 1. C6 Composition Loader | In progress (Root-only interim) | `firmware_loader.cpp` links real `object_api::CREATE_OBJECT`/`SPAWN`, uses correct `OBJECT_PRIVILEGED`/`OBJECT_UNPRIVILEGED`, rejects non-root parents and manifest count > 8 fail-closed. Full parent authority derivation remains incomplete. |
| 2. Loader Host Assertions | In progress (Unexecuted) | `tests/firmware_composition/test_manifest_loader.cpp` links production `//:firmware_loader` and mocks syscall seam with 46 assertions across 9 test cases; host test has not run. |
| 3. C6 `shizuku_firmware` Build | In progress (Unexecuted) | Archive bundling dedups `.a`/`.o`; `defs.bzl` sets `use_default_shell_env = True`; `build.sh` requires `.elf` and `.map`. Remote rbuild execution pending. |
| 4. `shizuku_sketch` Macro | In progress (Incomplete markers preserved) | Generates `:foo` binary and `:foo_obj` sketch object. Direct `:foo` reuse in `objects` of another firmware is incomplete; explicit sample roots preserved. |
| 5. Output Artifacts (.bin, .elf, .map) | In progress (Unexecuted) | No C6 `.bin`, `.elf`, `.map` or test pass exists. |
| 6. RP Firmware Migration | In progress (Preserved drafts) | RP-only files (`configs/rp_firmware.bzl`, `modules/pico_sdk_support/firmware/`, etc.) and `configs/BUILD.bazel` RP additions preserved intact. |

### Technical Boundaries & Incompleteness
- **Root-Only Interim Limitation**: The current C6 composition loader (`source/kernel/firmware_loader.cpp`) accepts only `parent_id == 1` (`root`). Non-root parent IDs are explicitly rejected and fail-closed. Full nested parent authority derivation is a pending specification item.
- **Direct Sketch Target Reuse Incomplete**: Direct `shizuku_sketch(':foo')` reuse in `objects` remains incomplete; consumers must reference `:foo_obj` explicitly.
- **Host Test Configuration (Pending Execution)**: Configured named job `test_manifest_loader` in `rbuild.toml` with explicit native host platform (`--platforms=@platforms//host`), C++23 (`--cxxopt=-std=c++23`), and assertion-enabled configuration (`--cxxopt=-UNDEBUG`) to prevent pulling default RP2350 platform/profile. Loader policy logic TU (`firmware_loader.cpp`) is separated from production syscall TU (`firmware_loader_syscall.cpp`) with stable SDK-independent `shizuku_loader_syscall_result_t`, allowing host link without ARCH::syscall or fake syscall stub. Execution remains pending.

---

## 3. Remote Worker & Environment Specification

- **Intended Worker**: `main-lxc`
- **Transport**: `ssh` (`--transport ssh`)
- **Remote Host / User**: `ishigakiyua@100.64.0.12` (`--remote ishigakiyua@100.64.0.12`)
- **Remote Worktree**: `objfw` (`--worktree objfw`)
- **Remote Agent Path**: `/home/ishigakiyua/.local/bin/rbuild`
- **Remote Build Root**: `/home/ishigakiyua/rbuild-work`
- **Remote Bazelisk Path**: `/home/ishigakiyua/.local/bin/bazelisk`
- **Remote Client ID**: `macbook`
- **Remote Build Parallelism**: `CMAKE_BUILD_PARALLEL_LEVEL=24`
- **Remote Action Environment Variables**:
  - `CMAKE_BUILD_PARALLEL_LEVEL=24`
  - `IDF_PATH=/home/ishigakiyua/.local/esp-idf-c5-v6.1`
  - `IDF_TOOLS_PATH=/home/ishigakiyua/.local/idf-tools-c5-v6.1`

---

## 4. Concrete Invocations (Named Jobs Only)

All remote invocations use the confirmed named job syntax `rbuild job <NAME>`. Generic Option B syntax (`rbuild ... bazel build`) has been removed as it is not verified via CLI.

```bash
# 1. Multi-object composition firmware (sample_obj + 2 sketches)
# Status: unexecuted / unverified
rbuild --transport ssh --remote ishigakiyua@100.64.0.12 --worktree objfw job build_esp32c6_composition

# 2. Single-sketch direct macro firmware
# Status: unexecuted / unverified
rbuild --transport ssh --remote ishigakiyua@100.64.0.12 --worktree objfw job build_esp32c6_direct_sketch

# 3. Host manifest loader policy & vocabulary test
# Status: unexecuted / pending execution on main-lxc (configured for @platforms//host, C++23, -UNDEBUG)
rbuild --transport ssh --remote ishigakiyua@100.64.0.12 --worktree objfw job test_manifest_loader
```

*(Every command above is currently unexecuted and unverified.)*

---

## 5. Desired Targets & Output Artifacts

### 1. Multi-Object Composition Target (`//tests/firmware_composition:sample_fw`)
- Binary: `bazel-bin/tests/firmware_composition/sample_fw.bin`
- ELF: `bazel-bin/tests/firmware_composition/sample_fw.elf`
- Map: `bazel-bin/tests/firmware_composition/sample_fw.map`
- Archive: `bazel-bin/tests/firmware_composition/libsample_fw_archive.a`

### 2. Single-Sketch Direct Macro Target (`//tests/firmware_composition:direct_sketch_fw`)
- Binary: `bazel-bin/tests/firmware_composition/direct_sketch_fw.bin`
- ELF: `bazel-bin/tests/firmware_composition/direct_sketch_fw.elf`
- Map: `bazel-bin/tests/firmware_composition/direct_sketch_fw.map`

### 3. Host Manifest Loader Test Target (`//tests/firmware_composition:test_manifest_loader`)
- Testlog: `bazel-testlogs/tests/firmware_composition/test_manifest_loader/test.log`

---

## 6. Dependency Closure, Uniqueness & Verification Checklist

1. **Symbol Uniqueness Verification**:
   - Inspect defined global symbols in `libsample_fw_archive.a`:
     ```bash
     # Status: unexecuted / unverified
     riscv32-esp-elf-nm -g --defined-only bazel-bin/tests/firmware_composition/libsample_fw_archive.a
     ```
   - Pass Criteria:
     - `shizuku_firmware_entry`: exactly 1 definition.
     - `shizuku_object_id_sample_obj`: exactly 1 definition.
     - `shizuku_object_id_sample_sketch_obj`: exactly 1 definition.
     - `shizuku_object_id_sample_sketch_obj_second`: exactly 1 definition.
     - `shizuku_object_id_count` & `shizuku_object_id_root`: exactly 1 definition (clean core separation without pulling legacy table from `//:internal_headers`).

2. **RAM Placement & Map Review**:
   - Inspect `sample_fw.map` and `direct_sketch_fw.map`:
     - Confirm trap handlers (`trap.cpp`, `c6_vectors.S`) reside in `.iram1.text` / designated RAM regions rather than Flash text.

3. **Runtime Safety Boundaries**:
   - **Strict NO DEVICE boundary**: No flash, serial, or hardware interaction is requested or authorized.
   - Remote execution must occur strictly on worker `main-lxc`.

---

## 7. Working Tree Dirty Inventory Disposition

- **Tracked Modified Files**:
  - `defs.bzl`: Added `use_default_shell_env = True` to `_shizuku_firmware_binary_impl` to pass `--action_env` to `build.sh`; restored C5 default kernel to `@shizuku//:shizuku_kernel_c5`; preserved `shizuku_sketch` incomplete notice.
  - `BUILD.bazel`: Isolated policy target `firmware_loader` (no configs:config dep); added `firmware_loader_syscall.cpp` alongside `firmware_loader.cpp` to `shizuku_kernel_c6_core`; removed from common `KERNEL_SRCS`; deleted unused `shizuku_kernel_c5_core` target.
  - `modules/esp_idf_support/firmware/build.sh`: Required `.elf` and `.map` to exist and fail with explicit diagnostics before copying.
  - `rbuild.toml`: Configured named job `test_manifest_loader` with explicit `--platforms=@platforms//host`, `--cxxopt=-std=c++23`, and `--cxxopt=-UNDEBUG`.
  - `configs/BUILD.bazel`: Mixes `core_headers` C6 closure change with RP profile additions (`rp2040_cmake_profile*`); preserved intact, not a clean C6-only file until hunks split.
  - `configs/kit.bzl`: RP profile substitution helper (`sdk_free = True`); preserved intact per review instructions.
  - `internal_headers/shizuku/templates/kernel_object.hpp`: Removal of incomplete runtime setters; preserved.
  - `modules/esp_idf_support/c6/early_boot.cpp`: Composition entry dispatch and weak selftest fallback; preserved.
  - `source/kernel/services.cpp`: Genuine `ARCH::syscall(YIELD)` ABI; preserved.
  - `tests/firmware_composition/BUILD.bazel`: Includes `test_manifest_loader` `cc_test` without `//configs:config` and root-only parent declarations; preserved.
  - `.handoff/object-firmware-composition.md`: Minimal status note appended.
- **Untracked Files**:
  - `internal_headers/shizuku/firmware_loader.h` & `firmware_loader.hpp`: Composition loader header C ABI with SDK-independent `shizuku_loader_syscall_result_t` and `shizuku_loader_seam_syscall` declaration.
  - `source/kernel/firmware_loader.cpp`: Pure policy TU (fail-closed root-only composition loader, no config.hpp or ARCH::syscall).
  - `source/kernel/firmware_loader_syscall.cpp`: Production syscall boundary TU bridging seam to `shizuku::ARCH::syscall`.
  - `tests/firmware_composition/test_manifest_loader.cpp`: 46 source asserts host test using `shizuku_loader_syscall_result_t` without config.hpp.
  - `configs/rp_firmware.bzl`, `modules/pico_sdk_support/firmware/`, `tests/rp2040_firmware/`: RP-only drafts preserved intact.
- **Ignored Files**:
  - `.handoff/rbuild-requests.md`: Matched by `.*` in global ignore (`~/.config/git/ignore:5`), force-added via `git add -f` as an authorized review checkpoint artifact.
  - `.handoff/object-firmware-composition-rp.md` & `.handoff/object-firmware-composition-sol.md`: Ignored by git (`.*`).
  - `.rbuild-out/`, `.rbuild/`, `bazel-*`: Build artifacts and workdirs ignored.
- **Commit Status**: Prepared before this commit and staged as a review checkpoint; remains pending only for remote build/test (does not claim no commit exists).

---

## 8. Required Return Evidence for Future rbuild Execution

When remote rbuild execution is subsequently performed by the runner, the return report must furnish the following exact evidence:
1. **Runtime HEAD**: The exact git commit SHA at time of execution.
2. **Git Status Showing Retained RP Diffs**: Clean baseline status showing retained RP diffs (`configs/kit.bzl`, `configs/rp_firmware.bzl`, `modules/pico_sdk_support/firmware/`, `tests/rp2040_firmware/`, and `configs/BUILD.bazel` RP profile additions) preserved in the working tree.
3. **Input-Diff / Snapshot Identity**: Explicit verification of the remote worktree/snapshot inputs matching the committed checkpoint state.
4. **Exact Command, Exit Code & Log Location**:
   - Exact invocation command string executed for each named job (`build_esp32c6_composition`, `build_esp32c6_direct_sketch`, `test_manifest_loader`).
   - Exact process exit code for each invocation.
   - Exact file path / URI of stdout/stderr and build execution logs.
5. **SHA-256 Checksums**:
   - Firmware binaries and debug artifacts: SHA-256 for `.bin`, `.elf`, `.map`, and archive (`libsample_fw_archive.a`).
   - Test execution: SHA-256 for test log (`test.log`) with assertion pass counts.

