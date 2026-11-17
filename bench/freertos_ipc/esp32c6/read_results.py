"""Decode a dump of frtos_ipc_bench_results (see main/main.c) and print a table.

usage: python3 -I read_results.py <dump.bin>
exit status: 0 done, 2 not finished, 3 bad magic/size.
"""
import struct
import sys

HDR = struct.Struct("<12I")  # magic version cpu_mhz tick_hz ovh count done reserved[5]
RES = struct.Struct("<32s13I")  # name n min mean max p50 p90 p99 param flags aux[4]
MAX_RESULTS = 64
FLAGS = ((1, "TIMEOUT"), (2, "ABORTED"), (4, "BADVAL"), (8, "NOCLK"))


def main(path):
    data = open(path, "rb").read()
    need = HDR.size + RES.size * MAX_RESULTS
    if len(data) < need:
        print("short dump: %d < %d" % (len(data), need))
        return 3
    h = HDR.unpack_from(data, 0)
    if h[0] != 0x46524254:
        print("bad magic 0x%08x" % h[0])
        return 3
    magic, ver, mhz, tick, ovh, count, done = h[:7]
    print("version=%d cpu=%dMHz tick=%dHz empty-loop=%d cyc count=%d done=%d noclk=%d"
          % (ver, mhz, tick, ovh, count, done, h[7] & 8 != 0))
    print("%-24s %6s %5s %8s %8s %8s %8s %8s %8s  %s  aux"
          % ("name", "param", "n", "min", "mean", "max", "p50", "p90", "p99", "flags"))
    for i in range(min(count, MAX_RESULTS)):
        f = RES.unpack_from(data, HDR.size + i * RES.size)
        name = f[0].split(b"\0")[0].decode("ascii", "replace")
        n, mn, mean, mx, p50, p90, p99, param, flags = f[1:10]
        fl = ",".join(t for b, t in FLAGS if flags & b) or "-"
        print("%-24s %6d %5d %8d %8d %8d %8d %8d %8d  %s  %s"
              % (name, param, n, mn, mean, mx, p50, p90, p99, fl, list(f[10:14])))
    return 0 if done == 1 else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
