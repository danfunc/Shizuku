#!/usr/bin/env python3
"""Audit RAM branches, following ARM linker veneers (not arbitrary BLX pointers).

Exit 1 for flash edges, 2 for unclassified indirect control flow. This proves
static placement only; indirect handlers/callbacks and flash data need review.
"""
import argparse
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('elf')
p.add_argument('--objdump', default='arm-none-eabi-objdump')
p.add_argument('--require-entry', action='store_true', help='require SVC entry and return stub in RAM')
p.add_argument('--all-ram', action='store_true', help='include SDK/OTA RAM code')
a = p.parse_args()
raw = subprocess.check_output([a.objdump, '-d', a.elf], text=True)
functions = {}
current = None
for line in raw.splitlines():
    m = re.match(r'^([0-9a-f]+) <(.+)>:', line)
    if m:
        current = int(m[1], 16)
        functions[current] = [m[2], []]
    elif current is not None:
        functions[current][1].append(line)

def ram(x): return 0x20000000 <= x < 0x20082000

def flash(x): return 0x10000000 <= (x & ~1) < 0x11000000

def hot(name):
    return ('shizuku' in name and ('templates' in name or 'svc' in name or
        'return_stub' in name or 'current_context' in name or
        'restore_region_window' in name or 'arm_pending_step' in name or
        'handler_entry_point' in name))

edges, indirect, audited = [], [], 0
if a.require_entry:
    for suffix in ('svc_entry', 'return_stub'):
        found = [(addr, name) for addr, (name, _) in functions.items()
                 if name in ('shizuku_armv8m_' + suffix, 'shizuku_armv6m_' + suffix)]
        if not found or any(not ram(addr) for addr, _ in found):
            print('MISSING_RAM_ROOT', suffix)
            raise SystemExit(1)
for addr, (name, lines) in functions.items():
    if not ram(addr) or (not a.all_ram and not hot(name)): continue
    audited += 1
    for line in lines:
        m = re.search(r'\s(b(?:l|lx)?(?:\.w|\.n)?|b[a-z]{2}(?:\.w|\.n)?)\s+([0-9a-f]+)\s+<', line)
        if m:
            dst = int(m[2], 16)
            if flash(dst): edges.append((name, line.strip()))
            elif dst in functions and 'veneer' in functions[dst][0]:
                for v in functions[dst][1]:
                    literal = re.search(r'\.word\s+0x([0-9a-f]+)', v)
                    if literal and flash(int(literal[1], 16)):
                        edges.append((name, line.strip()+' -> '+v.strip()))
        # BX LR and BX r3 in exception restore are intentional returns.
        if re.search(r'\bblx\s+r\d+', line): indirect.append((name, line.strip()))
for name, line in edges: print('FLASH', name, line)
for name, line in indirect: print('INDIRECT', name, line)
print(f'audited_functions={audited} flash_edges={len(edges)} indirect_calls={len(indirect)}')
raise SystemExit(1 if edges else 2 if indirect or not audited else 0)
