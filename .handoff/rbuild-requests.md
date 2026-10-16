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

---

## 9. Current Request Correction and Opus Run Gate (2026-10-16)

This section is the current operator instruction and supersedes the command, transport, host, and worktree selections recorded in Sections 3–4 above. Those earlier values remain a dated historical snapshot; do not use them for a current invocation.

### Confirmed latest state

- The previous host test log `.handoff/rbuild_test_manifest_loader_20261016.log:27-33` records a successful Bazel build (10 actions), `PASSED`, and 1/1 test pass, followed by rbuild artifact collection failure because `bazel-testlogs/tests/firmware_composition/test_manifest_loader/test.log` escaped the worktree. `.handoff/rbuild_test_manifest_loader_20261016.exit` contains `1`. The test passed; the rbuild job did not complete successfully.
- `rbuild.toml` no longer requests that escaping `test.log` artifact. This config correction has not been rerun.
- Opus explicitly confirmed that 32 GB has been freed on the worker and ESP-IDF work may resume. The old `objfw` Bazel output base has been deleted, so the next firmware invocation is expected to be a cold build. This is an operator-provided state, not a measurement made in this checkout.
- Previous remote output displayed a `main` worktree/path where the request expected `objfw` (the output included `main` and a remote `.../main` path). Before accepting any new result, verify the selected `objfw` worktree and the synchronized input snapshot. A command line containing `--worktree objfw` alone does not prove the worker actually used the requested worktree or the intended source snapshot.
- Current worker selection is `main-lxc` from `rbuild.toml`; the user's rbuild configuration resolves the worker through MagicDNS. The worker SDK installation path and current toolchain availability have not been independently measured here.

### Current pending commands

The following commands are for Opus to execute through the configured rbuild worker. All are **unexecuted and pending**. They use the configured `main-lxc` worker and `objfw` worktree; no transport, remote host, or IP is specified here.

```bash
rbuild --worktree objfw job build_esp32c6_composition
rbuild --worktree objfw job build_esp32c6_direct_sketch
```

Targets and expected artifacts remain those listed in Section 5. The intended verification includes checking unique global ID definitions in the composition archive, reviewing the produced maps, and recording exact command, exit, log path, input snapshot identity, and SHA-256 for each produced artifact. No build, archive, map, or device proof is claimed by this request update.

### Current host-test status

The existing `test_manifest_loader` host test has source/log evidence of 1/1 passing as noted above, but its rbuild invocation exited 1 during artifact collection. The artifact list correction in `rbuild.toml` is pending a future rbuild retry if Opus chooses to verify the corrected job configuration. Do not rerun Bazel locally. Any retry command must use the configured worker/worktree form `rbuild --worktree objfw job test_manifest_loader` and be reported as unexecuted until it is actually run.

### Operator report gate

For each future job, report the exact invoked command, process exit, raw log location, selected worker and actual worktree path, source snapshot/HEAD identity, produced artifact paths and SHA-256, and any test pass count. If the observed worker path again says `main` rather than `objfw`, stop acceptance at that gate and report the mismatch before claiming a matching-source build.

### External consumer fixture status

The build-free fixture at `tests/external_composition/` models a separate MODULE consumer with a relative `../..` Shizuku override and two C6 firmware targets that share one object compile target. Its Python source regression and `nm` text checker tests passed in the local static-only invocation recorded in `.handoff/external-composition-source.md`; this does not establish Bazel module resolution, object compile-once behavior, or actual archive symbol uniqueness. No rbuild job for the nested MODULE is configured. The named-job execution root and synchronized snapshot behavior for this nested consumer remain unknown; do not add an external-module rbuild invocation until that root/cwd contract is confirmed. Real archive inspection remains pending after Opus's C6 builds and a verified input/worktree match.

---

## 10. Retry Execution After C6 build.sh Project Directory Fix (2026-10-16)

### Failed Initial Run Evidence
- **Job 1**: `build_esp32c6_composition` (`//tests/firmware_composition:sample_fw`)
  - **Result**: Exit 1 (recorded in `.handoff/rbuild_build_esp32c6_composition_20261016.log` and `.handoff/rbuild_build_esp32c6_composition_20261016.exit`).
  - **Failure Point**: Action `[43 / 44] Linking ESP-IDF firmware binary for @@//tests/firmware_composition:sample_fw` failed during `idf.py set-target`.
  - **Root Cause**: `_shizuku_firmware_binary_impl` sets `executable = ctx.executable.build_sh` (exec-transition tool). In Bazel exec transitions, the executable wrapper resides under `bazel-out/<config>-exec-.../bin/modules/esp_idf_support/firmware/build_sh`. `build.sh` resolved project directory via `SRC=$(cd -P "$(dirname "$0")" && pwd)`, which pointed to `bazel-out/.../bin/...` where `CMakeLists.txt`, `main/`, and `sdkconfig.defaults` do not exist.
- **Job 2**: `build_esp32c6_direct_sketch` (`//tests/firmware_composition:direct_sketch_fw`)
  - **Result**: Exit 1 (recorded in `.handoff/rbuild_build_esp32c6_direct_sketch_20261016.log` and `.handoff/rbuild_build_esp32c6_direct_sketch_20261016.exit`).
  - **Failure Point**: Action `[12 / 13] Linking ESP-IDF firmware binary for @@//tests/firmware_composition:direct_sketch_fw` failed during `idf.py set-target`.
  - **Root Cause**: Identical exec/bin missing-source-project/set-target failure. Both failure logs/exits were read and verified by Sol.

### Source Corrections Applied
1. **`modules/esp_idf_support/firmware/build.sh`**:
   - Accepts optional 6th argument `PROJECT_DIR` (with fallback to environment variables `SHIZUKU_IDF_PROJECT_DIR` and `IDF_PROJECT_PATH`, and legacy fallback to `$(cd -P "$(dirname "$0")" && pwd)`).
   - Validates `test -f "$SRC/CMakeLists.txt"` and `test -d "$SRC/main"` before `cd "$SRC"`, failing immediately with clear diagnostics if missing.
   - Preserves C5/C6 target handling, GCC banner verification, and deterministic `BUILD_DIR` handling.
2. **`defs.bzl` (`_shizuku_firmware_binary_impl`)**:
   - Locates top-level `CMakeLists.txt` in `ctx.files.esp32c6_project` and derives `project_dir`.
   - Passes `ctx.attr.build_dir if ctx.attr.build_dir else ""` as positional argument 5, and `project_dir` as positional argument 6 to `build.sh`.
3. **Verification**:
   - Build-free mock regression test `tests/firmware_composition/test_c6_build_sh_mock.py` executed: 8/8 tests passed (exit 0), logged in `.handoff/c6-build-sh-mock-20261016.log`.
   - Simulates exec directory invocation without project dir argument (fails cleanly with expected diagnostic) and with explicit project dir / env fallback (succeeds, changes working directory to project dir, copies `.bin`, `.elf`, `.map`).

### MagicDNS Retry Commands (Unexecuted & Pending for Opus)
The following commands are for Opus to execute on the remote worker via MagicDNS. Do NOT run locally.

```bash
# 1. Multi-object composition firmware (retry after build.sh project dir fix)
rbuild --worktree objfw job build_esp32c6_composition

# 2. Single-sketch direct macro firmware (retry after build.sh project dir fix)
rbuild --worktree objfw job build_esp32c6_direct_sketch
```

### Remote Routing Caveat & Verification Requirement
- **Known Forwarding Bug**: In both failed runs (`.handoff/rbuild_build_esp32c6_composition_20261016.log:1,4` and `.handoff/rbuild_build_esp32c6_direct_sketch_20261016.log:1,4`), `rbuild job` connected with `worktree='main'` and locked at `/home/ishigakiyua/rbuild-work/macbook/Shizuku-objfw/main` despite the client invocation passing `--worktree objfw`.
- **Out of Scope**: This remote worktree routing issue is an established forwarding bug in `rbuild job` passing `--worktree` to child processes on the remote worker and is out of scope for repository source changes.
- **Verification Rule**: Do not claim `objfw` was used or that `main` is cold. When reviewing results, Opus must inspect the actual remote worktree path and verify the synchronized input snapshot directly against the committed checkpoint state before accepting build artifacts.

---

## 11. Latest Superseding Acceptance Table (2026-10-16 Checkpoint)

This table reflects the current accepted state across all workstreams and supersedes previous historical status tables:

| Workstream / Item | State | Evidence / Status / Remaining Gate |
|---|---|---|
| 1. Host Manifest Loader Test (`test_manifest_loader`) | PASS 1/1 (Artifact collection exit 1) | Target build and test passed (1/1) in `.handoff/rbuild_test_manifest_loader_20261016.log`; original rbuild exited 1 on escaping `test.log` artifact. `rbuild.toml` artifact config corrected in `a8bce91`; retry not yet run. |
| 2. RP2040 Firmware Source Checkpoint | Staged in `33752f5` (Build PENDING) | Pure Python static regression tests 16/16 passed (`tests/rp2040_firmware/test_rp2040_firmware.sh`); real CMake/Pico SDK compilation, ELF, MAP, and UF2 generation remain pending. |
| 3. External Consumer Fixtures (`tests/external_composition/`) | Staged in `a8bce91` (Remote rbuild root unresolved) | Standalone fixture & text symbol checker pure Python 7/7 passed; real nested Bazel MODULE resolution, archive deduplication, compile-once behavior, and external rbuild execution root remain unresolved. |
| 4. C6 / C5 `build.sh` Project Dir Fix | Reviewed & Accepted (Real rbuild retries PENDING) | Positional argument 6 project directory handling in `defs.bzl` and `modules/esp_idf_support/firmware/build.sh`; mock regression suite 8/8 passed (`.handoff/c6-build-sh-mock-20261016.log`). Real rbuild jobs `build_esp32c6_composition` and `build_esp32c6_direct_sketch` remain PENDING. |
| 5. Firmware Composition Incompleteness Markers | Incomplete by Design | Root-only interim loader limitation (`parent_id == 1` only; non-root authority derivation incomplete); direct public sketch provider reuse (`shizuku_sketch(':foo')` in `objects`) incomplete; consumers must reference `:foo_obj` explicitly. |
| 6. Remote Invocations | Unexecuted / Pending | Existing latest MagicDNS commands (`rbuild --worktree objfw job build_esp32c6_composition`, `build_esp32c6_direct_sketch`) remain unchanged and pending for Opus. |

