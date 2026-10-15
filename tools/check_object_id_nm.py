#!/usr/bin/env python3
"""Check global object-ID definitions in GNU/LLVM nm output from an archive."""

import argparse
import collections
import re
import sys

SYMBOL_PATTERN = re.compile(r"\s([A-Za-z])\s+(shizuku_object_id_[A-Za-z_][A-Za-z0-9_]*)$")
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", action="append", required=True,
                        help="expected object name (without shizuku_object_id_ prefix); repeat as needed")
    args = parser.parse_args()
    expected = ["shizuku_object_id_" + name for name in args.expected]
    errors, definitions = check_nm_output(sys.stdin.read(), expected)
    if errors:
        for error in errors:
            print("ERROR: " + error, file=sys.stderr)
        return 1
    for symbol in sorted(definitions):
        print("OK: %s defined once" % symbol)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
