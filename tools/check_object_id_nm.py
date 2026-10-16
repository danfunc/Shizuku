#!/usr/bin/env python3
"""Check global object-ID definitions in GNU/LLVM nm output from an archive."""

import argparse
import collections
import re
import sys

SYMBOL_PATTERN = re.compile(r"\s([A-Za-z])\s+(shizuku_object_id_[A-Za-z_][A-Za-z0-9_]*)$")
ENTRY_PATTERN = re.compile(r"(?:^|\s)([A-Za-z])\s+([A-Za-z_][A-Za-z0-9_]*)$")
POSIX_PATTERN = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z])\s+")
# nm uses uppercase types for externally visible symbols and lowercase types
# for local symbols. U is an undefined external reference, not a definition.
DEFINED_GLOBAL_TYPES = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ") - {"U"}


def check_nm_output(nm_output, expected_symbols):
    """Return diagnostics for missing, unexpected, or multiply-defined ID symbols."""
    definitions = collections.Counter()
    for line in nm_output.splitlines():
        match = SYMBOL_PATTERN.search(line)
        if match and match.group(1) in DEFINED_GLOBAL_TYPES:
            definitions[match.group(2)] += 1

    expected = set(expected_symbols)
    actual = set(definitions)
    errors = []
    for symbol in sorted(expected - actual):
        errors.append("missing definition: " + symbol)
    for symbol in sorted(actual - expected):
        errors.append("unexpected definition: " + symbol)
    for symbol, count in sorted(definitions.items()):
        if count != 1:
            errors.append("expected one definition of %s, got %d" % (symbol, count))
    return errors, definitions


def check_entry_symbols(nm_output, expected_entries):
    """Return diagnostics for missing or multiply-defined C ABI entry symbols."""
    definitions = collections.Counter()
    for line in nm_output.splitlines():
        sym = None
        sym_type = None
        match = SYMBOL_PATTERN.search(line)
        if match:
            sym_type, sym = match.group(1), match.group(2)
        else:
            m_posix = POSIX_PATTERN.search(line.strip())
            if m_posix:
                sym, sym_type = m_posix.group(1), m_posix.group(2)
            else:
                m_gen = ENTRY_PATTERN.search(line)
                if m_gen:
                    sym_type, sym = m_gen.group(1), m_gen.group(2)
        if sym and sym in expected_entries and sym_type in DEFINED_GLOBAL_TYPES:
            definitions[sym] += 1

    expected = set(expected_entries)
    actual = set(definitions)
    errors = []
    for symbol in sorted(expected - actual):
        errors.append("missing entry definition: " + symbol)
    for symbol, count in sorted(definitions.items()):
        if count != 1:
            errors.append("expected one definition of entry %s, got %d" % (symbol, count))
    return errors, definitions


def check_undefined_symbols(nm_output, rejected_symbols):
    """Return diagnostics if any rejected symbol appears undefined ('U') in nm output."""
    found_undefined = collections.Counter()
    for line in nm_output.splitlines():
        for sym in rejected_symbols:
            if sym in line:
                tokens = line.split()
                if "U" in tokens and sym in tokens:
                    found_undefined[sym] += 1
    errors = []
    for symbol, count in sorted(found_undefined.items()):
        errors.append("forbidden undefined reference found for %s (%d occurrences)" % (symbol, count))
    return errors, found_undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", action="append", default=[],
                        help="expected object name (without shizuku_object_id_ prefix); repeat as needed")
    parser.add_argument("--expected-entry", action="append", default=[],
                        help="expected C ABI entry function name; repeat as needed")
    parser.add_argument("--reject-undefined", action="append", default=[],
                        help="symbol that must not appear undefined ('U') in nm output; repeat as needed")
    args = parser.parse_args()

    if not args.expected and not args.expected_entry and not args.reject_undefined:
        parser.error("At least one of --expected, --expected-entry, or --reject-undefined must be provided.")

    content = sys.stdin.read()
    all_errors = []

    if args.expected:
        expected = ["shizuku_object_id_" + name for name in args.expected]
        errors, definitions = check_nm_output(content, expected)
        all_errors.extend(errors)
        if not errors:
            for symbol in sorted(definitions):
                print("OK: %s defined once" % symbol)

    if args.expected_entry:
        entry_errors, entry_defs = check_entry_symbols(content, args.expected_entry)
        all_errors.extend(entry_errors)
        if not entry_errors:
            for symbol in sorted(entry_defs):
                print("OK: entry %s defined once" % symbol)

    if args.reject_undefined:
        undef_errors, _ = check_undefined_symbols(content, args.reject_undefined)
        all_errors.extend(undef_errors)
        if not undef_errors:
            for symbol in sorted(args.reject_undefined):
                print("OK: %s not undefined" % symbol)

    if all_errors:
        for error in all_errors:
            print("ERROR: " + error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
