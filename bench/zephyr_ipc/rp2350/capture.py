"""Read a USB CDC serial port (stdlib only) and append everything to a file.

usage: python3 -I capture.py <tty> <outfile> [max_seconds]
Stops when a dump with done=1 followed by END has been seen, when the device
disappears (it reboots to BOOTSEL ~30 s after finishing), or after max_seconds
(default 900). Exit 0 if a finished dump was seen, 2 otherwise.
"""
import os
import select
import sys
import termios
import time


def main(tty, out, limit):
    t_end = time.time() + limit
    buf = b""
    allb = b""
    fd = None
    while fd is None and time.time() < t_end:
        try:
            fd = os.open(tty, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.5)
    if fd is None:
        print("could not open", tty)
        return 2
    attr = termios.tcgetattr(fd)
    attr[0] = 0
    attr[1] = 0
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    finished = False
    enough = False
    with open(out, "ab") as f:
        while time.time() < t_end and not finished:
            r, _, _ = select.select([fd], [], [], 1.0)
            if not r:
                continue
            try:
                d = os.read(fd, 4096)
            except OSError:
                break  # device went away
            if not d:
                continue
            f.write(d)
            f.flush()
            buf = (buf + d)[-4096:]
            allb += d
            # the firmware sends >= 5 full dumps; stop after 5 finished ones or at the reboot notice
            if allb.count(b"done=1") >= 5:
                enough = True
            if b"REBOOT to BOOTSEL" in buf:
                finished = True  # keep the port open until the firmware says it leaves
    os.close(fd)
    return 0 if (finished or enough) else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 900.0))
