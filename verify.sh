#!/bin/zsh
set -u
cd /Users/ishigakiyua/github/Shizuku
echo "########## 1. git status (壊していないことの確認) ##########"
git status --short
echo
echo "########## 2. ビルド ##########"
bazelisk build //firmware:shizuku_uf2 2>&1 | grep -E "error:|warning:|Build completed|FAILED" | head -20
echo
echo "########## 3. 実機のシリアル読み取り (20秒、焼かない) ##########"
cat > /tmp/readser_verify.py <<'PYEOF'
import os, glob, select, time, threading, sys
devs = sorted(glob.glob("/dev/cu.usbmodem*"))
if not devs:
    print("!! シリアルデバイスが見つからない"); sys.exit(0)
print("# devices:", devs)
out = {}
def reader(dev, dur):
    try:
        fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    except OSError as e:
        out[dev] = f"(open failed: {e})"; return
    end = time.time() + dur; buf = b""
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if r:
            try: buf += os.read(fd, 8192)
            except (BlockingIOError, OSError): pass
    os.close(fd); out[dev] = buf.decode("utf-8", "replace")
ts = [threading.Thread(target=reader, args=(d, 20.0)) for d in devs]
for t in ts: t.start()
for t in ts: t.join()
for d in devs:
    txt = out.get(d, "")
    for line in txt.replace("\r", "\n").split("\n"):
        if "COST]" in line or "selftest=" in line:
            print(line)
PYEOF
python3 /tmp/readser_verify.py
echo
echo "########## 4. call_cost.cpp の noinline を数える ##########"
grep -c "__attribute__((noinline))" source/selftest/call_cost.cpp
echo "--- op_ 関数の一覧 ---"
grep -n "uintptr_t op_" source/selftest/call_cost.cpp
echo
echo "########## 5. git status (再確認: 変わっていないこと) ##########"
git status --short
