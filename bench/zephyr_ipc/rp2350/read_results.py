"""Turn a CDC capture of the Zephyr IPC bench into the same table as the FreeRTOS one.

usage: python3 -I read_results.py <capture.txt>
exit status: 0 all rows recovered and done, 2 not finished / rows missing, 3 nothing usable.

Every line the firmware sends ends with " *<crc32 hex>" (zlib.crc32 of everything before
it). Lines whose check value does not match (cut or interleaved by the USB buffer) are
dropped. The firmware repeats the full dump (BEGIN, R rows, F rows, END) at least 5 times,
so the table is built from all valid R lines of all dumps: for a row name the first
valid copy is kept, and the row order is the order of first appearance.
Rows named *_m are loop means (min/max/pXX are 0: not available); *_d are distributions.
"""
import re
import sys
import zlib

FLAGS = ((1, "TIMEOUT"), (2, "ABORTED"), (4, "BADVAL"), (8, "NOCLK"))
ROW = re.compile(
    r"^R (\S+) param=(\d+) n=(\d+) min=(\d+) mean=(\d+) max=(\d+) p50=(\d+) p90=(\d+) "
    r"p99=(\d+) flags=(\d+) aux=(\d+),(\d+),(\d+),(\d+)$")
HDR = re.compile(r"^BEGIN magic=(\d+) version=(\d+) cpu=(\d+)MHz tick=(\d+)Hz "
                 r"ovh_priv=(\d+) ovh_user=(\d+) count=(\d+) done=(\d+)$")


def valid_lines(text):
    """Yield (line without check value) for each line whose crc matches; count the others."""
    good, bad = [], 0
    for raw in text.replace("\r", "").split("\n"):
        raw = raw.strip()
        if not raw:
            continue
        if " *" not in raw:
            bad += 1
            continue
        body, _, crc = raw.rpartition(" *")
        try:
            ok = int(crc, 16) == zlib.crc32(body.encode("ascii", "replace")) & 0xFFFFFFFF
        except ValueError:
            ok = False
        if ok:
            good.append(body)
        else:
            bad += 1
    return good, bad


def main(path):
    text = open(path, "r", errors="replace").read()
    lines, bad = valid_lines(text)
    rows, order, hdr, ndump = {}, [], None, 0
    faults, last_p = [], None
    for l in lines:
        m = HDR.match(l)
        if m:
            h = tuple(int(x) for x in m.groups())
            if hdr is None or h[7] >= hdr[7]:
                hdr = h
            if h[7] == 1:
                ndump += 1
            continue
        m = ROW.match(l)
        if m:
            name = m.group(1)
            if name not in rows:
                rows[name] = (name,) + tuple(int(x) for x in m.groups()[1:])
                order.append(name)
            continue
        if l.startswith("F ") and l not in faults:
            faults.append(l)
        elif l.startswith("P "):
            last_p = l
    if not rows and not faults and last_p is None:
        print("nothing usable in %s (%d damaged/unchecked lines dropped)" % (path, bad))
        return 3
    if hdr:
        _, ver, mhz, tick, ovh_p, ovh_u, count, done = hdr
    else:
        ver = mhz = tick = ovh_p = ovh_u = count = done = 0
    print("version=%d cpu=%dMHz tick=%dHz ovh_priv=%d ovh_user=%d cyc count=%d done=%d "
          "rows=%d full_dumps=%d dropped_lines=%d"
          % (ver, mhz, tick, ovh_p, ovh_u, count, done, len(rows), ndump, bad))
    print("%-24s %6s %5s %8s %8s %8s %8s %8s %8s  %s  aux"
          % ("name", "param", "n", "min", "mean", "max", "p50", "p90", "p99", "flags"))
    for name in order:
        r = rows[name]
        _, param, n, mn, mean, mx, p50, p90, p99, flags = r[:10]
        fl = ",".join(t for b, t in FLAGS if flags & b) or "-"
        print("%-24s %6d %5d %8d %8d %8d %8d %8d %8d  %s  %s"
              % (name, param, n, mn, mean, mx, p50, p90, p99, fl, list(r[10:14])))
    if last_p:
        print("last progress line: %s" % last_p)
    for l in faults:
        print(l)
    missing = count - len(rows) if hdr else 0
    if missing > 0:
        print("MISSING %d of %d rows (no valid copy in any dump)" % (missing, count))
    return 0 if (done == 1 and missing <= 0) else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
