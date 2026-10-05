#!/usr/bin/env python3
"""BLE OTA 送信ツール (Shizuku 用)

機能:
  scan : 名前 "Shizuku UART" を探す
  nus  : NUS RX へ文字列を書き、TX notify を表示する
  send : OTA サービスへ XNOR/XNOU 送信、ペーシング制御、NEEDSEQ 再送ループ、
         切断時自動レジューム、done 確認、--commit で XNOC

オプション:
  --selfcheck : ヘッダ生成・CRC16/32・raw deflate・NEEDSEQ パース・分割ロジックの自己検査を実行
"""

import argparse
import asyncio
import datetime
import os
import re
import struct
import sys
import time
import zlib
from typing import List, Optional, Set, Tuple

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.characteristic import BleakGATTCharacteristic
    from bleak.exc import BleakError
except ImportError:
    BleakClient = None
    BleakScanner = None
    BleakError = Exception

# ===========================================================================
#  GATT UUID 定義 (ble_uart.gatt と一致)
# ===========================================================================
SHIZUKU_DEVICE_NAME = "Shizuku UART"

NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
NUS_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  # Central -> Peripheral (Write)
NUS_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  # Peripheral -> Central (Notify)

OTA_SERVICE_UUID = "6E402001-B5A3-F393-E0A9-E50E24DCCA9E"
OTA_RX_CHAR_UUID = "6E402002-B5A3-F393-E0A9-E50E24DCCA9E"  # Central -> Peripheral (Write / Write Without Response)

FLASH_SECTOR_SIZE = 4096
CHUNK_HDR_BYTES = 16
CHUNK_MAGIC = b"XNCK"
QUERY_SEQ = 0xFFFF
RESET_SEQ = 0xFFFE
DEFAULT_PACKET_SIZE = 244  # ATT MTU 247 - 3
DEFAULT_PACKET_DELAY = 0.003  # 1 パケット送信後のスリープ秒 (3ms)
DEFAULT_CHUNK_GAP = 0.055  # 1 チャンク完了後の flash 書き込み待機秒 (55ms: erase 38ms + prog 12ms)


def log(msg: str) -> None:
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    print(f"[{now}] {msg}", flush=True)


# ===========================================================================
#  CRC 計算 (ota.cpp と完全一致)
# ===========================================================================
def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (init 0xFFFF, poly 0x1021, no xorout, MSB first)"""
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def make_chunk_header(seq: int, payload_len: int, payload_crc32: int) -> bytes:
    """XNCK 16バイトヘッダを組み立てる (先頭14BのCRC16付き)"""
    hdr14 = struct.pack("<4sHHIH", CHUNK_MAGIC, seq, payload_len, payload_crc32, 0)
    c16 = crc16_ccitt(hdr14)
    return hdr14 + struct.pack("<H", c16)


def make_query_header() -> bytes:
    """QUERY フレーム (seq=0xFFFF, len=0)"""
    return make_chunk_header(QUERY_SEQ, 0, 0)


def make_reset_header() -> bytes:
    """RESET フレーム (seq=0xFFFE, len=0)"""
    return make_chunk_header(RESET_SEQ, 0, 0)


def make_init_header(magic: str, total_len: int, image_crc32: int) -> bytes:
    """12バイトの転送初期化ヘッダ (XNOR / XNOU / XNOC)"""
    return struct.pack("<4sII", magic.encode("ascii"), total_len, image_crc32)


def compress_sector_raw_deflate(sector_bytes: bytes) -> bytes:
    """RFC1951 raw deflate (wbits=-15, ヘッダ・フッタなし) で圧縮"""
    comp = zlib.compressobj(level=9, method=zlib.DEFLATED, wbits=-15)
    return comp.compress(sector_bytes) + comp.flush()


def parse_needseq_lines(lines: List[str]) -> List[int]:
    """NEEDSEQ 行から欠損 seq 番号のリストを抽出する

    例:
      "NEEDSEQ 0,1,3-5" -> [0, 1, 3, 4, 5]
      "NEEDSEQ 10 12-14" -> [10, 12, 13, 14]
    """
    needed: List[int] = []
    for line in lines:
        if not line.startswith("NEEDSEQ"):
            continue
        parts = line[len("NEEDSEQ"):].strip().replace(",", " ").split()
        for part in parts:
            if not part:
                continue
            if "-" in part:
                tokens = part.split("-", 1)
                try:
                    start = int(tokens[0])
                    end = int(tokens[1])
                    needed.extend(range(start, end + 1))
                except ValueError:
                    pass
            else:
                try:
                    needed.append(int(part))
                except ValueError:
                    pass
    return sorted(list(set(needed)))


def drain_queue(q: asyncio.Queue) -> List[str]:
    """キュー内の既存要素をすべて取り出す"""
    items = []
    while not q.empty():
        try:
            items.append(q.get_nowait())
        except asyncio.QueueEmpty:
            break
    return items


# ===========================================================================
#  受信判定ヘルパー (純関数: テスト可能)
# ===========================================================================
def is_transfer_error_line(line: str) -> bool:
    """デバイス側エラー通知 (crc MISMATCH, FAILED, erase failed) の判定"""
    return any(err in line for err in ["crc MISMATCH", "FAILED", "erase failed"])


def check_done_line(line: str, expected_len: int, expected_crc32: int) -> bool:
    """done 通知行の検証を行う純関数
    成功条件:
      - 'done:' かつ 'OK' を含む
      - '<total> bytes' が expected_len と一致
      - 'crc=<8桁hex>' が expected_crc32 と一致 (小文字hex)
      - 'crc MISMATCH', 'FAILED', 'erase failed' を含まない
    """
    if is_transfer_error_line(line):
        return False
    if "done:" not in line or "OK" not in line:
        return False
    if f"{expected_len} bytes" not in line:
        return False
    expected_crc_hex = f"{expected_crc32 & 0xFFFFFFFF:08x}"
    if f"crc={expected_crc_hex}" not in line.lower():
        return False
    return True


def check_nus_response(received_lines: List[str]) -> bool:
    """NUS 応答判定: 1 行以上の通知を受信していれば True、0 行なら False"""
    return len(received_lines) > 0


def check_ready_timeout(ready_ok: bool) -> bool:
    """ready 応答待ち判定: タイムアウト(False)時は失敗(False)"""
    return bool(ready_ok)


def check_commit_line(line: str) -> bool:
    """commit 受理通知判定: 'commit:' を含みエラー通知でなければ True"""
    if "commit rejected" in line or "could not start" in line:
        return False
    return "commit:" in line



# ===========================================================================
#  自己検査 (--selfcheck)
# ===========================================================================
def run_selfcheck() -> bool:
    log("=== [SELFCHECK] 自己検査を開始 ===")

    # 1. CRC16/CCITT-FALSE
    c_empty = crc16_ccitt(b"")
    assert c_empty == 0xFFFF, f"crc16 empty expected 0xFFFF, got {hex(c_empty)}"
    c_123456789 = crc16_ccitt(b"123456789")
    assert c_123456789 == 0x29B1, f"crc16 123456789 expected 0x29B1, got {hex(c_123456789)}"
    log("  [PASS] CRC16/CCITT-FALSE 計算検算")

    # 2. XNOR チャンクヘッダ
    dummy_payload = b"HelloWorld12345678"
    p_len = len(dummy_payload)
    p_crc = zlib.crc32(dummy_payload) & 0xFFFFFFFF
    hdr = make_chunk_header(seq=42, payload_len=p_len, payload_crc32=p_crc)
    assert len(hdr) == 16, f"Header length must be 16, got {len(hdr)}"
    assert hdr[:4] == b"XNCK", f"Magic must be XNCK, got {hdr[:4]}"
    seq_val, len_val, crc_val, rsv_val, crc16_val = struct.unpack("<HHIHH", hdr[4:])
    assert seq_val == 42, f"Seq mismatch: {seq_val}"
    assert len_val == p_len, f"Len mismatch: {len_val}"
    assert crc_val == p_crc, f"CRC mismatch: {crc_val}"
    assert rsv_val == 0, f"Rsv mismatch: {rsv_val}"
    assert crc16_val == crc16_ccitt(hdr[:14]), "CRC16 self-validation failed"
    log("  [PASS] XNOR チャンクヘッダ構造・CRC16検算")

    # 3. QUERY / RESET / COMMIT ヘッダ
    q_hdr = make_query_header()
    assert len(q_hdr) == 16
    assert struct.unpack("<H", q_hdr[4:6])[0] == QUERY_SEQ
    assert struct.unpack("<H", q_hdr[6:8])[0] == 0
    assert struct.unpack("<H", q_hdr[14:16])[0] == crc16_ccitt(q_hdr[:14])

    r_hdr = make_reset_header()
    assert len(r_hdr) == 16
    assert struct.unpack("<H", r_hdr[4:6])[0] == RESET_SEQ
    assert struct.unpack("<H", r_hdr[14:16])[0] == crc16_ccitt(r_hdr[:14])

    c_hdr = make_init_header("XNOC", 12345, 0xABCDEF01)
    assert len(c_hdr) == 12
    assert c_hdr[:4] == b"XNOC"
    log("  [PASS] QUERY / RESET / COMMIT ヘッダ生成検算")

    # 4. raw deflate 圧縮・展開検算
    test_raw = os.urandom(FLASH_SECTOR_SIZE)
    compressed = compress_sector_raw_deflate(test_raw)
    assert compressed[:2] != b"\x78\x9c", "Raw deflate must not contain zlib header (0x78 0x9c)"
    decompressed = zlib.decompress(compressed, -15)
    assert decompressed == test_raw, "Decompressed data must match original raw sector"
    log("  [PASS] raw deflate (RFC1951 wbits=-15) 圧縮・展開等価性検算")

    # 5. NEEDSEQ パース検算
    sample_lines = [
        "NEED n=7 of=10 ok=3 bad=0 r=1",
        "NEEDSEQ 0,1, 3-5",
        "NEEDSEQ 8-9",
        "NEEDEND",
    ]
    parsed = parse_needseq_lines(sample_lines)
    expected = [0, 1, 3, 4, 5, 8, 9]
    assert parsed == expected, f"NEEDSEQ parse mismatch: got {parsed}, want {expected}"
    log("  [PASS] NEEDSEQ 範囲記法・カンマ区切りパース検算")

    # 6. パケット分割ロジック検算
    test_bytes = b"X" * 1000
    pkt_size = 244
    pkts = [test_bytes[i : i + pkt_size] for i in range(0, len(test_bytes), pkt_size)]
    assert len(pkts) == 5
    assert len(pkts[0]) == 244
    assert len(pkts[-1]) == 24
    assert b"".join(pkts) == test_bytes
    log("  [PASS] パケット分割境界検算")

    # 7. drain_queue 検算
    async def _test_q():
        q: asyncio.Queue = asyncio.Queue()
        q.put_nowait("a")
        q.put_nowait("b")
        drained = drain_queue(q)
        assert drained == ["a", "b"]
        assert q.empty()

    asyncio.run(_test_q())
    log("  [PASS] キュー・ドレイン動作検算")

    # 8. done 行判定検算 (意味のある失敗経路テスト)
    valid_done = "done: 12345 bytes crc=abcdef01 OK (staged at 0x100000)"
    assert check_done_line(valid_done, 12345, 0xABCDEF01) is True, "valid done line must return True"
    assert check_done_line("done: 12345 bytes crc=11223344 OK (staged at 0x100000)", 12345, 0xABCDEF01) is False, "crc mismatch must return False"
    assert check_done_line("done: 99999 bytes crc=abcdef01 OK (staged at 0x100000)", 12345, 0xABCDEF01) is False, "length mismatch must return False"
    assert check_done_line("done: 12345 bytes crc=abcdef01 NG", 12345, 0xABCDEF01) is False, "missing OK must return False"
    assert check_done_line("done: crc MISMATCH: expected 0xabcdef01, got 0x11223344", 12345, 0xABCDEF01) is False, "crc MISMATCH must return False"
    assert check_done_line("FAILED: erase failed", 12345, 0xABCDEF01) is False, "erase failed must return False"
    log("  [PASS] done 行判定検算 (正常系 / CRC不一致 / 長さ不一致 / OK欠落 / crc MISMATCH / エラー通知)")

    # 9. nus 応答判定検算
    assert check_nus_response([]) is False, "nus empty response must return False"
    assert check_nus_response(["reply from dev\n"]) is True, "nus non-empty response must return True"
    log("  [PASS] NUS 応答判定検算 (無応答時は False / 応答時は True)")

    # 10. ready タイムアウト判定検算
    assert check_ready_timeout(False) is False, "ready timeout (False) must return False (failure)"
    assert check_ready_timeout(True) is True, "ready received (True) must return True"
    log("  [PASS] ready タイムアウト判定検算 (タイムアウト時は失敗)")

    # 11. commit 受理判定検算
    assert check_commit_line("commit: 12345 bytes -> 0x100000 (4 sectors), no return") is True, "valid commit must return True"
    assert check_commit_line("commit rejected: size") is False, "commit rejected must return False"
    assert check_commit_line("commit could not start (rc=-1)") is False, "commit could not start must return False"
    log("  [PASS] commit 受理判定検算 (正常 / rejected / start失敗)")

    log("=== [SELFCHECK] ALL TESTS PASSED (全 11 項目合格) ===")
    return True


# ===========================================================================
#  BLE デバイス探索 (scan)
# ===========================================================================
async def do_scan(target_name: str = SHIZUKU_DEVICE_NAME, timeout: float = 5.0) -> Optional[str]:
    if BleakScanner is None:
        log("ERROR: bleak がインストールされていません")
        return None

    log(f"BLE スキャン開始 (timeout={timeout}s, 検索対象: '{target_name}')...")
    devices = await BleakScanner.discover(timeout=timeout)
    found_addr = None
    log(f"スキャン完了: {len(devices)} 台のデバイスを検出")

    for d in devices:
        name = d.name or "(Unknown)"
        rssi = getattr(d, "rssi", "N/A")
        is_target = (name == target_name) or (target_name.lower() in name.lower())
        mark = " [* TARGET]" if is_target else ""
        log(f"  - {d.address} | Name: {name} | RSSI: {rssi}{mark}")
        if is_target and found_addr is None:
            found_addr = d.address

    if found_addr:
        log(f"ターゲット発見: {found_addr} ('{target_name}')")
    else:
        log(f"警告: ターゲット '{target_name}' は見つかりませんでした")
    return found_addr


# ===========================================================================
#  NUS コマンド
# ===========================================================================
async def do_nus(address: str, text: str, timeout: float = 5.0) -> bool:
    if BleakClient is None:
        log("ERROR: bleak がインストールされていません")
        return False

    log(f"NUS: {address} に接続中...")
    async with BleakClient(address) as client:
        if not client.is_connected:
            log("ERROR: 接続に失敗しました")
            return False
        log("接続成功。NUS TX notify を購読開始...")

        rx_lines: List[str] = []

        def notification_handler(_sender: BleakGATTCharacteristic, data: bytearray):
            txt = data.decode("utf-8", errors="replace")
            log(f"[NUS TX notify] {txt.strip()}")
            rx_lines.append(txt)

        await client.start_notify(NUS_TX_CHAR_UUID, notification_handler)

        if not text.endswith("\n"):
            text += "\n"
        log(f"NUS RX へ書き込み: {repr(text)}")
        await client.write_gatt_char(NUS_RX_CHAR_UUID, text.encode("utf-8"), response=True)

        log(f"応答待機中 ({timeout}s)...")
        await asyncio.sleep(timeout)
        await client.stop_notify(NUS_TX_CHAR_UUID)

    if not check_nus_response(rx_lines):
        log("NUS: 往復未確認 (タイムアウトまでに NUS TX 通知を受信できませんでした)")
        return False

    log(f"NUS: 受信確認 ({len(rx_lines)} 件の通知を受信):")
    for l in rx_lines:
        log(f"  <- {l.strip()}")
    log("NUS 完了")
    return True


# ===========================================================================
#  通知待機ヘルパー
# ===========================================================================
async def wait_for_notify_matching(
    queue: asyncio.Queue[str],
    pattern: str,
    timeout: float = 3.0,
    collected_lines: Optional[List[str]] = None,
) -> bool:
    """正規表現パターンに合致する通知行が届くまで待機する"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        rem = timeout - (time.time() - t0)
        if rem <= 0:
            break
        try:
            line = await asyncio.wait_for(queue.get(), timeout=min(rem, 0.5))
            if collected_lines is not None:
                collected_lines.append(line)
            if re.search(pattern, line):
                return True
        except asyncio.TimeoutError:
            continue
    return False


# ===========================================================================
#  OTA 送信 (send)
# ===========================================================================
async def do_send(
    address: str,
    bin_path: str,
    mode: str = "xnor",
    commit: bool = False,
    packet_size: int = DEFAULT_PACKET_SIZE,
    packet_delay: float = DEFAULT_PACKET_DELAY,
    chunk_gap: float = DEFAULT_CHUNK_GAP,
    window_size: int = 0,
    use_response: bool = False,
    max_rounds: int = 15,
    auto_reconnect: bool = True,
) -> bool:
    if BleakClient is None:
        log("ERROR: bleak がインストールされていません")
        return False

    if not os.path.exists(bin_path):
        log(f"ERROR: バイナリファイルが存在しません: {bin_path}")
        return False

    with open(bin_path, "rb") as f:
        image_data = f.read()

    total_len = len(image_data)
    image_crc32 = zlib.crc32(image_data) & 0xFFFFFFFF
    log(f"ファームウェア読込: {bin_path} ({total_len} bytes, CRC32={hex(image_crc32)})")

    # 生バイナリのベクタテーブル簡易検査
    if total_len >= 8:
        sp, reset_vector = struct.unpack("<II", image_data[:8])
        log(f"  Vector table check: SP=0x{sp:08x}, Reset=0x{reset_vector:08x}")
        if (sp & 0xFF000000) != 0x20000000:
            log("  警告: 初期 SP が SRAM 領域 (0x20xxxxxx) を指していません")
        if (reset_vector & 0xFF000000) != 0x10000000:
            log("  警告: Reset Vector が Flash 領域 (0x10xxxxxx) を指していません")

    nchunks = (total_len + FLASH_SECTOR_SIZE - 1) // FLASH_SECTOR_SIZE
    log(f"モード: {mode.upper()}, セクタ数: {nchunks} (1 セクタ = {FLASH_SECTOR_SIZE} bytes)")
    log(f"ペーシング設定: packet_delay={packet_delay*1000:.1f}ms, chunk_gap={chunk_gap*1000:.1f}ms, window_size={window_size}")

    # XNOR チャンクの事前圧縮準備
    chunks: List[Tuple[int, bytes]] = []
    if mode == "xnor":
        for seq in range(nchunks):
            at = seq * FLASH_SECTOR_SIZE
            sector = image_data[at : at + FLASH_SECTOR_SIZE]
            payload = compress_sector_raw_deflate(sector)
            p_crc = zlib.crc32(payload) & 0xFFFFFFFF
            chdr = make_chunk_header(seq, len(payload), p_crc)
            chunks.append((seq, chdr + payload))
        total_comp = sum(len(c[1]) - CHUNK_HDR_BYTES for c in chunks)
        log(f"XNOR 圧縮完了: 総ペイロード {total_comp} bytes (元サイズ比 {total_comp/total_len*100:.1f}%)")

    # 送信管理変数
    needed_seqs: List[int] = list(range(nchunks))
    acknowledged_seqs: Set[int] = set()
    transfer_done = False

    reconnect_attempts = 3 if auto_reconnect else 1

    for attempt in range(1, reconnect_attempts + 1):
        if transfer_done:
            break

        log(f"接続試行 {attempt}/{reconnect_attempts}: {address} へ接続中...")
        client = BleakClient(address)
        try:
            await client.connect(timeout=10.0)
            if not client.is_connected:
                log(f"警告: 接続に失敗しました (attempt {attempt})")
                await asyncio.sleep(1.0)
                continue

            log("接続成功")

            # ペアリング呼び出し (macOS では内部管理されるため例外は無視可)
            try:
                if hasattr(client, "pair"):
                    paired = await client.pair()
                    log(f"client.pair() 戻り値: {paired}")
            except Exception as e:
                log(f"ペアリング情報 (OS側で管理されている場合は無視可): {e}")

            notify_queue: asyncio.Queue[str] = asyncio.Queue()

            def notification_handler(_sender: BleakGATTCharacteristic, data: bytearray):
                text = data.decode("utf-8", errors="replace")
                for line in text.splitlines(keepends=True):
                    clean_line = line.strip()
                    if clean_line:
                        log(f"[DEVICE] {clean_line}")
                    notify_queue.put_nowait(line)

            await client.start_notify(NUS_TX_CHAR_UUID, notification_handler)
            log("NUS TX notify 購読開始")
            # 接続パラメータ更新などの安定化を待つ
            await asyncio.sleep(0.5)

            # ---------------------------------------------------------------
            # 1. 初期化ハンドシェイク (初回のみ RESET + INIT)
            # ---------------------------------------------------------------
            if attempt == 1:
                # 転送前 RESET 送信 (response=True で確実に送達)
                log("転送前 RESET フレーム (16B) を送信して IDLE へ復帰させます...")
                reset_frame = make_reset_header()
                drain_queue(notify_queue)
                await client.write_gatt_char(OTA_RX_CHAR_UUID, reset_frame, response=True)
                reset_ok = await wait_for_notify_matching(notify_queue, r"reset", timeout=2.0)
                if not check_ready_timeout(reset_ok):
                    log("ERROR: デバイスからの 'reset' 応答タイムアウト (失敗)")
                    return False

                # 初期化ヘッダ送信 (XNOR / XNOU)
                if mode == "xnor":
                    init_hdr = make_init_header("XNOR", total_len, image_crc32)
                elif mode == "xnou":
                    init_hdr = make_init_header("XNOU", total_len, image_crc32)
                else:
                    log(f"ERROR: 未知のモード: {mode}")
                    return False

                log(f"初期化ヘッダ送信: magic={mode.upper()}, total={total_len}, crc32={hex(image_crc32)}")
                drain_queue(notify_queue)
                await client.write_gatt_char(OTA_RX_CHAR_UUID, init_hdr, response=True)
                ready_ok = await wait_for_notify_matching(notify_queue, r"ready", timeout=10.0)
                if not check_ready_timeout(ready_ok):
                    log("ERROR: デバイスからの 'ready' 応答タイムアウト (失敗)")
                    return False
            else:
                # 再接続時: すでに OTA セッションが残っているため、RESET ではなく QUERY で同期
                log("再接続後: QUERY フレームを送信して現在の受領状態を確認します...")
                drain_queue(notify_queue)
                query_frame = make_query_header()
                await client.write_gatt_char(OTA_RX_CHAR_UUID, query_frame, response=True)
                # NEED 応答を収集
                round_lines = []
                saw_need = await wait_for_notify_matching(notify_queue, r"NEED", timeout=4.0, collected_lines=round_lines)
                if saw_need:
                    new_needed = parse_needseq_lines(round_lines)
                    if new_needed:
                        needed_seqs = new_needed
                        log(f"再接続同期成功: 残り {len(needed_seqs)} チャンクから再開")

            # ---------------------------------------------------------------
            # 2. XNOR チャンク送信ループ
            # ---------------------------------------------------------------
            if mode == "xnor":
                t_total_start = time.time()

                for round_idx in range(1, max_rounds + 1):
                    if transfer_done or not needed_seqs:
                        break

                    log(f"=== ラウンド {round_idx}/{max_rounds}: 送信対象 {len(needed_seqs)} チャンク ===")
                    t_round_start = time.time()
                    chunks_sent_in_round = 0

                    for seq_idx, seq in enumerate(needed_seqs):
                        _, frame_bytes = chunks[seq]
                        n_pkts = (len(frame_bytes) + packet_size - 1) // packet_size

                        # チャンク内のパケット送信
                        off = 0
                        while off < len(frame_bytes):
                            sub = frame_bytes[off : off + packet_size]
                            await client.write_gatt_char(OTA_RX_CHAR_UUID, sub, response=use_response)
                            off += len(sub)
                            if packet_delay > 0:
                                await asyncio.sleep(packet_delay)

                        chunks_sent_in_round += 1

                        # ★重要ペーシング: 1 チャンク完了後の flash 消去・書き込み待機
                        #   ターゲット側の write_sector() (4KB 消去 38ms + 書込み 12ms) の完了を待つ
                        if chunk_gap > 0:
                            await asyncio.sleep(chunk_gap)

                        # 進捗表示 (5 チャンクごと、または最後)
                        if (seq_idx + 1) % 5 == 0 or (seq_idx + 1) == len(needed_seqs):
                            pct = (seq_idx + 1) / len(needed_seqs) * 100.0
                            log(f"  [round {round_idx}] 送信進捗: {seq_idx + 1}/{len(needed_seqs)} ({pct:.1f}%) | seq={seq} ({len(frame_bytes)}B, {n_pkts}pkts)")

                        # ウィンドウ同期 (window_size > 0 の場合)
                        if window_size > 0 and chunks_sent_in_round >= window_size and (seq_idx + 1) < len(needed_seqs):
                            log(f"  -- ウィンドウ境界 ({chunks_sent_in_round} チャンク送出): QUERY 同期を実行 --")
                            chunks_sent_in_round = 0
                            drain_queue(notify_queue)
                            q_frame = make_query_header()
                            await client.write_gatt_char(OTA_RX_CHAR_UUID, q_frame, response=True)
                            w_lines = []
                            await wait_for_notify_matching(notify_queue, r"NEED", timeout=3.0, collected_lines=w_lines)

                    elapsed_round = time.time() - t_round_start
                    log(f"ラウンド {round_idx} 送信完了 ({elapsed_round:.2f}s)。QUERY フレームを送信...")

                    # -------------------------------------------------------
                    # QUERY 送信と NEED 応答収集
                    # -------------------------------------------------------
                    drain_queue(notify_queue)
                    query_frame = make_query_header()

                    # QUERY 送信と応答収集 (最大 3 回リトライ)
                    round_lines: List[str] = []
                    missing_count = None
                    saw_needend = False

                    for query_try in range(1, 4):
                        await client.write_gatt_char(OTA_RX_CHAR_UUID, query_frame, response=True)
                        t0 = time.time()
                        while time.time() - t0 < 6.0 and not saw_needend:
                            try:
                                line = await asyncio.wait_for(notify_queue.get(), timeout=1.5)
                                round_lines.append(line)
                                if is_transfer_error_line(line):
                                    log(f"ERROR: デバイス側エラー通知を受信: {line.strip()}")
                                    return False
                                if "NEEDEND" in line:
                                    saw_needend = True
                                if check_done_line(line, total_len, image_crc32):
                                    transfer_done = True
                                m = re.search(r"NEED n=(\d+) of=(\d+)", line)
                                if m:
                                    missing_count = int(m.group(1))
                            except asyncio.TimeoutError:
                                # タイムアウトしても全体時間 6.0s まで継続
                                continue

                        if saw_needend or transfer_done or missing_count is not None:
                            break
                        log(f"  警告: QUERY 応答タイムアウト。再送します ({query_try}/3)...")
                        await asyncio.sleep(0.5)

                    if transfer_done:
                        elapsed_total = time.time() - t_total_start
                        log(f"★ ファームウェア転送完了！ (device reports done: OK, 全所要時間: {elapsed_total:.2f}s)")
                        break

                    if missing_count is not None:
                        if missing_count == 0:
                            log("欠損 0。完了行 (done: OK) を待機します...")
                            done_lines: List[str] = []
                            await wait_for_notify_matching(notify_queue, r"done:", timeout=3.0, collected_lines=done_lines)
                            for l in done_lines:
                                if is_transfer_error_line(l):
                                    log(f"ERROR: デバイス側エラー通知を受信: {l.strip()}")
                                    return False
                                if check_done_line(l, total_len, image_crc32):
                                    transfer_done = True
                                    break
                            if not transfer_done:
                                for l in drain_queue(notify_queue):
                                    if is_transfer_error_line(l):
                                        log(f"ERROR: デバイス側エラー通知を受信: {l.strip()}")
                                        return False
                                    if check_done_line(l, total_len, image_crc32):
                                        transfer_done = True
                                        break
                            if transfer_done:
                                elapsed_total = time.time() - t_total_start
                                log(f"★ ファームウェア転送完了！ (device reports done: OK, 全所要時間: {elapsed_total:.2f}s)")
                                break
                            else:
                                log("ERROR: 欠損 0 ですが正しい done: 行 (サイズ・CRC一致) を受信できませんでした")
                                return False
                        else:
                            parsed_needed = parse_needseq_lines(round_lines)
                            if parsed_needed:
                                needed_seqs = parsed_needed
                            else:
                                log("警告: NEEDSEQ 行が取得できませんでした。前回の欠損リストを維持します")
                            log(f"欠損検出: {missing_count} 個 (再送 seq 数: {len(needed_seqs)})")
                    else:
                        log("警告: デバイスから NEED 応答が得られませんでした。前回の欠損リストを維持します")

                if not transfer_done:
                    log("ERROR: 転送が完了しませんでした (制限ラウンド超過またはエラー)")
                    return False

            elif mode == "xnou":
                # 無圧縮送信 (XNOU)
                log("無圧縮データを送信中...")
                off = 0
                while off < total_len:
                    sub = image_data[off : off + packet_size]
                    await client.write_gatt_char(OTA_RX_CHAR_UUID, sub, response=use_response)
                    off += len(sub)
                    if packet_delay > 0:
                        await asyncio.sleep(packet_delay)
                log("全データ送信完了。結果待機中...")
                xnou_lines: List[str] = []
                await wait_for_notify_matching(notify_queue, r"done:", timeout=5.0, collected_lines=xnou_lines)
                for l in xnou_lines:
                    if is_transfer_error_line(l):
                        log(f"ERROR: デバイス側エラー通知を受信: {l.strip()}")
                        return False
                    if check_done_line(l, total_len, image_crc32):
                        transfer_done = True
                        break
                if not transfer_done:
                    for l in drain_queue(notify_queue):
                        if is_transfer_error_line(l):
                            log(f"ERROR: デバイス側エラー通知を受信: {l.strip()}")
                            return False
                        if check_done_line(l, total_len, image_crc32):
                            transfer_done = True
                            break
                if not transfer_done:
                    log("ERROR: XNOU 送信完了後に正しい done: 行 (サイズ・CRC一致) を受信できませんでした")
                    return False
                log("★ XNOU ファームウェア転送完了！ (device reports done: OK)")

            if not transfer_done:
                log("ERROR: 転送完了が確認されませんでした")
                return False

            # ---------------------------------------------------------------
            # 3. コミット (--commit)
            # ---------------------------------------------------------------
            if commit:
                log("=== --commit 指定: XNOC 要求を送出 ===")
                commit_hdr = make_init_header("XNOC", total_len, image_crc32)
                drain_queue(notify_queue)
                await client.write_gatt_char(OTA_RX_CHAR_UUID, commit_hdr, response=True)
                log("XNOC 要求を送出しました。デバイスからの commit 受理通知を待機中...")
                commit_lines: List[str] = []
                await wait_for_notify_matching(notify_queue, r"commit:", timeout=5.0, collected_lines=commit_lines)
                valid_commit = any(check_commit_line(l) for l in commit_lines)
                if not valid_commit:
                    for l in drain_queue(notify_queue):
                        if check_commit_line(l):
                            valid_commit = True
                            break
                if not valid_commit:
                    log("XNOC 要求を送出・デバイスから commit 受理通知(commit: を含む行)を未確認 (失敗)")
                    log("再起動後の新ビルド起動確認は外部シリアルの [BOOT] build: で別途必要")
                    return False
                log("XNOC 要求を送出・デバイスから commit 受理通知(commit: を含む行)を確認")
                log("再起動後の新ビルド起動確認は外部シリアルの [BOOT] build: で別途必要")

            await client.stop_notify(NUS_TX_CHAR_UUID)
            log("OTA プロセス正常終了 (SUCCESS)")
            return True

        except (BleakError, asyncio.TimeoutError, OSError) as e:
            log(f"通信例外発生 ({type(e).__name__}: {e})")
            if attempt < reconnect_attempts:
                log(f"切断を検知しました。{attempt + 1} 回目の再接続を試みます (2秒待機)...")
                await asyncio.sleep(2.0)
            else:
                log("再試行上限に達しました")
                return False
        finally:
            try:
                if client.is_connected:
                    await client.disconnect()
            except Exception:
                pass

    return transfer_done


# ===========================================================================
#  メインエントリポイント
# ===========================================================================
def main():
    parser = argparse.ArgumentParser(description="Shizuku BLE OTA Host Tool")
    parser.add_argument("--selfcheck", action="store_true", help="ヘッダ生成・CRC・パース等の単体自己テストを実行")

    subparsers = parser.add_subparsers(dest="command")

    # scan
    sub_scan = subparsers.add_parser("scan", help="BLE デバイス探索")
    sub_scan.add_argument("--name", default=SHIZUKU_DEVICE_NAME, help="探索するデバイス名")
    sub_scan.add_argument("--timeout", type=float, default=5.0, help="スキャン時間 (秒)")

    # nus
    sub_nus = subparsers.add_parser("nus", help="NUS (Nordic UART Service) 往復テスト")
    sub_nus.add_argument("text", help="NUS RX へ書き込む文字列")
    sub_nus.add_argument("--address", help="対象デバイスの BLE アドレス (省略時は自動探索)")
    sub_nus.add_argument("--timeout", type=float, default=3.0, help="応答待機時間 (秒)")

    # send
    sub_send = subparsers.add_parser("send", help="ファームウェア OTA 送信")
    sub_send.add_argument("bin", help="送信する生バイナリファイル (.bin)")
    sub_send.add_argument("--mode", choices=["xnor", "xnou"], default="xnor", help="送信モード (既定: xnor)")
    sub_send.add_argument("--commit", action="store_true", help="転送成功後に XNOC でフラッシュ反映と再起動を実行")
    sub_send.add_argument("--address", help="対象デバイスの BLE アドレス (省略時は自動探索)")
    sub_send.add_argument("--packet-delay", type=float, default=DEFAULT_PACKET_DELAY, help=f"パケット間スリープ秒 (既定: {DEFAULT_PACKET_DELAY})")
    sub_send.add_argument("--chunk-delay", type=float, default=None, help="パケット間スリープ秒 (--packet-delay のエイリアス)")
    sub_send.add_argument("--chunk-gap", type=float, default=DEFAULT_CHUNK_GAP, help=f"1 チャンク完了後の flash 書き込み待ち秒 (既定: {DEFAULT_CHUNK_GAP})")
    sub_send.add_argument("--window-size", type=int, default=0, help="同期ウィンドウのチャンク数 (既定: 0 = 全チャンク送出後に QUERY)")
    sub_send.add_argument("--response", action="store_true", help="チャンクデータ送信に Write with Response を使用")
    sub_send.add_argument("--packet-size", type=int, default=DEFAULT_PACKET_SIZE, help="1 パケットのバイト数 (既定: 244)")
    sub_send.add_argument("--max-rounds", type=int, default=15, help="再送ラウンド最大数 (既定: 15)")
    sub_send.add_argument("--no-reconnect", action="store_true", help="切断時の自動再接続を無効化")

    args = parser.parse_args()

    if args.selfcheck:
        success = run_selfcheck()
        sys.exit(0 if success else 1)

    if not args.command:
        parser.print_help()
        sys.exit(1)

    if args.command == "scan":
        addr = asyncio.run(do_scan(target_name=args.name, timeout=args.timeout))
        sys.exit(0 if addr is not None else 1)

    elif args.command == "nus":
        addr = args.address
        if not addr:
            addr = asyncio.run(do_scan(timeout=3.0))
            if not addr:
                log("ERROR: デバイスが見つかりませんでした")
                sys.exit(1)
        ok = asyncio.run(do_nus(address=addr, text=args.text, timeout=args.timeout))
        sys.exit(0 if ok else 1)

    elif args.command == "send":
        addr = args.address
        if not addr:
            addr = asyncio.run(do_scan(timeout=3.0))
            if not addr:
                log("ERROR: デバイスが見つかりませんでした")
                sys.exit(1)

        # --chunk-delay が指定されていた場合は packet_delay として反映
        pkt_delay = args.chunk_delay if args.chunk_delay is not None else args.packet_delay

        ok = asyncio.run(
            do_send(
                address=addr,
                bin_path=args.bin,
                mode=args.mode,
                commit=args.commit,
                packet_size=args.packet_size,
                packet_delay=pkt_delay,
                chunk_gap=args.chunk_gap,
                window_size=args.window_size,
                use_response=args.response,
                max_rounds=args.max_rounds,
                auto_reconnect=not args.no_reconnect,
            )
        )
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
