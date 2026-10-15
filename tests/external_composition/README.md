# External composition ID regression source note

Date: 2026-10-16
Base HEAD: `72db1072b3ac88b6d0f5511e317ae3f1b790b1b4`
Evidence type: source inspection and build-free Python checks only.

## Composition and ID path

- `defs.bzl:17-23` defines the `ShizukuObjectInfo` provider with an explicit `object_name`; `defs.bzl:51-87` creates a compile unit and adds `@shizuku//:core_headers` by default. The same-chip compile-once behavior is documented at `defs.bzl:59-62`, but this note contains no Bazel action evidence that it occurred.
- `defs.bzl:147-174` reads each composition object's provider name and rejects duplicate names inside that composition. `defs.bzl:197-215` calls `tools/gen_object_ids.py` for the composition's ID definition source. `defs.bzl:241-270` assigns runtime IDs from 2 and sets root parent ID to 1.
- `tools/gen_object_ids.py:94-125` sorts list inputs, starts object IDs at 2, rejects repeated names in one generator call, emits extern C ABI references and one definition per name. `tools/object_ids.c.in:5-7` keeps the legacy exported root symbol at value 0. The legacy root value 0 and runtime root parent ID 1 are separate contracts.
- `defs.bzl:304-360` merges static libraries and direct objects into a firmware archive, deduplicating by full artifact path. It does not drop inputs based only on basename. `defs.bzl:652-685` wraps sketch entry points as ordinary named objects; `defs.bzl:687-711` records that reusing the public `:foo` sketch binary as an object remains incomplete and callers should use its `:foo_obj` target.

## Durable external consumer fixture

`tests/external_composition/MODULE.bazel:1-7` declares a separate consumer module and resolves Shizuku through the relative `../..` local path override; this avoids the previous scratch fixture's absolute machine path. `tests/external_composition/BUILD.bazel:6-22` declares one shared object and two sketch objects. `tests/external_composition/BUILD.bazel:24-36` places that same shared object in two separate firmware compositions, each with one sketch. The corresponding generator inputs are `consumer_alpha.names` and `consumer_beta.names`.

`tools/check_object_id_nm.py:12-30` checks global defined `shizuku_object_id_*` symbols parsed from `nm -g --defined-only` output. It requires the expected set, rejects unexpected ID definitions, and requires exactly one definition per actual symbol; it has no basename-based filter. Its CLI is intended to receive real archive symbol output, for example:

```bash
nm -g --defined-only <firmware-archive.a> | python3 tools/check_object_id_nm.py --expected root --expected count --expected shared_sensor --expected status_sketch
```

That archive command is an unexecuted future check. The fixture has not been analyzed by Bazel, compiled, linked, or inspected as an archive. The source-only tests use generator-produced C definition files and representative `nm` text, not actual firmware archives. Therefore archive/global-symbol uniqueness in a produced binary remains unverified.

## Checks performed

- `python3 tests/external_composition/test_id_composition.py` (cwd repository root): exit 0; 7 tests passed. Assertions at `tests/external_composition/test_id_composition.py:18-27` check the relative module override and one shared object target reused by two firmware declarations. Assertions at `:46-63` check generated per-composition ID definitions, one definition per name, and the stable shared ID. Assertions at `:65-75` accept distinct ID symbols even when archive member basenames match; `:77-89` reject a repeated actual ID definition; `:91-107` exclude undefined and local symbols; and `:109-114` reject a missing expected and unexpected actual global ID. Captured output and exits are in `.handoff/external-source-tests-20261016.log`.
- The test also invokes `tools/gen_object_ids.py` through Python only; it does not invoke a compiler, Bazel, rbuild, or device tooling.
- `python3 -c 'import tomllib; d=tomllib.load(open("rbuild.toml","rb")); j=d["jobs"]["test_manifest_loader"]; assert j["artifacts"] == []; assert j["worker"] == "main-lxc"; print("TOML_OK test_manifest_loader artifacts=0 worker=main-lxc")'` (cwd repository root): exit 0; output `TOML_OK test_manifest_loader artifacts=0 worker=main-lxc`.

## rbuild and remaining gates

The previous test run log `.handoff/rbuild_test_manifest_loader_20261016.log:27-33` records a successful host build and 1/1 passing test followed by artifact collection failure because the requested `test.log` escaped the worktree; `.handoff/rbuild_test_manifest_loader_20261016.exit` records exit 1. `rbuild.toml:72-83` now has an empty artifact list for that host test. The configuration correction has not been rerun.

The current `rbuild` request and its pending C6 commands are in `.handoff/rbuild-requests.md` Section 9. Prior remote output showed `main` where the request expected `objfw`; worker root and synchronized input identity must be confirmed before accepting future results. The external fixture has a nested `MODULE.bazel`, but the current rbuild job configuration does not establish how a named job changes its execution root into that nested module. No external-module rbuild job is declared or verified. Treat the fixture's rbuild execution root, synchronized snapshot behavior, and dependency mapping as unknown until the actual worker input is shown.

## RP header closure patch

The separately assigned subpackage-header gate is addressed in `modules/pico_sdk_support/objects/BUILD.bazel:10-18` and `modules/pico_sdk_support/BUILD.bazel:35-45`: the child package exports its own internal headers, and the parent filegroup references that label instead of trying to glob through the child package boundary. This should place `shizuku/objects/usb_cdc.hpp` in the `extra_headers` inputs assembled in `defs.bzl:493-500` and `defs.bzl:453-460`. No RP build was run, so the input closure and compilation result remain pending source review and worker execution.

## Not claimed

No local Bazel/build, rbuild/remote command, SDK probe, serial/flash, or device operation was run in this turn. No archive, ELF, UF2, map, link result, external MODULE analysis result, or hardware behavior is claimed. Full parent-authority derivation and direct public sketch target provider reuse remain incomplete.
