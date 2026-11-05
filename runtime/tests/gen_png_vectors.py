#!/usr/bin/env python3
"""fox/image/png.cpp 的测试向量生成器 —— 生成 runtime/tests/fox_png_vectors.hpp。

为什么要生成而不是手写
--------------------
inflate 和 PNG 的正确性证据是"zlib 自己产出的流转回来一模一样"，而
zlib 在 Python 标准库里。手写 300 字节的十六进制数组既不可复核也不可复现；
这个脚本重跑一次就能重新得到同样的头文件。

为什么向量里要有 hand-crafted 的流
--------------------------------
zlib 不会为了测试去产出一个"距离 32769（越过窗口）"的非法流，也不会
在 32768 这个边界上恰好用上距离 32768。这两种情形只能自己用位写入器造：
固定 Huffman 表是 RFC1951 里唯一不需要动态表就能手写的那张。

用法：  python3 runtime/tests/gen_png_vectors.py > runtime/tests/fox_png_vectors.hpp
"""

import struct
import sys
import zlib

# ──────────────────────────────────────────────────────────────────────
# 确定性图案：C++ 侧用同一个公式重新生成，所以不需要把几十 KB 的明文
# 塞进头文件。改公式必须两边一起改 —— 这正是它被写成"公式"而不是
# "数据"的原因：两个副本会漂移，而公式只有一份。
# ──────────────────────────────────────────────────────────────────────

def pattern(n):
    """与 fox_png_test.cpp 的 gen_pattern() 必须逐字节一致。"""
    out = bytearray()
    x = 0x2545F491
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out.append((x >> 17) & 0xFF)
    return bytes(out)

def ab_pattern(total):
    """ABAB... 的前 `total` 字节（'AB' 重复）—— 手写流里用距离 2 的游程。"""
    return (b"AB" * ((total + 1) // 2))[:total]


# ──────────────────────────────────────────────────────────────────────
# 手写 DEFLATE：固定 Huffman（RFC1951 §3.2.6）+ 位写入器
# ──────────────────────────────────────────────────────────────────────

class BitWriter:
    """DEFLATE 的位序：填充位 LSB-first，Huffman 码 MSB-first。

    这两句话是 RFC1951 里最容易写错的地方，也是手写向量唯一的价值：
    如果这个脚本把码位序写反了，inflate 会解出错的东西（而不是"碰巧通过"）。
    """

    def __init__(self):
        self.bits = []

    def raw(self, val, n):
        """普通整数：低位在前。"""
        for i in range(n):
            self.bits.append((val >> i) & 1)

    def code(self, code, n):
        """Huffman 码：高位先发。"""
        for i in range(n - 1, -1, -1):
            self.bits.append((code >> i) & 1)

    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            b = 0
            for j, bit in enumerate(self.bits[i:i + 8]):
                b |= bit << j
            out.append(b)
        return bytes(out)


def lit_code(v):
    """固定 Huffman 字面量/长度码 → (code, nbits)。"""
    if v <= 143:
        return (0x30 + v, 8)
    if v <= 255:
        return (0x190 + v - 144, 9)
    if v <= 279:
        return (0x0 + v - 256, 7)
    return (0xC0 + v - 280, 8)


LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
             12289, 16385, 24577]
DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13]


def emit_literal(bw, byte):
    c, n = lit_code(byte)
    bw.code(c, n)


def emit_match(bw, length, dist):
    """长度/距离对（调用者负责合法性：dist <= 已输出字节数）。"""
    li = max(i for i in range(29) if LEN_BASE[i] <= length)
    c, n = lit_code(257 + li)
    bw.code(c, n)
    bw.raw(length - LEN_BASE[li], LEN_EXTRA[li])

    di = max(i for i in range(30) if DIST_BASE[i] <= dist)
    bw.code(di, 5)  # 固定距离码就是 5 位的序号本身
    bw.raw(dist - DIST_BASE[di], DIST_EXTRA[di])


def emit_eob(bw):
    c, n = lit_code(256)
    bw.code(c, n)


def wrap_zlib(deflate, plain):
    ad = zlib.adler32(plain) & 0xFFFFFFFF
    return b"\x78\x01" + deflate + struct.pack(">I", ad)


def handcrafted_window_boundary():
    """距离恰好 32768（LZ77 窗口边界）的合法流。

    输出 = 'AB' 重复 16384 次（32768 字节）+ 距离 32768、长度 3 的一段。

    做法：写 2 个字面量 'AB'，再用 126 个「长度 258、距离 2」的游程把它
    撑到 32768（每个只花约 15 位），最后一次回溯距离恰好是 32768 ——
    多一个字节就越界，少一个字节就不用碰这个边界。
    """
    bw = BitWriter()
    bw.raw(1, 1)  # BFINAL
    bw.raw(1, 2)  # BTYPE = 01 (fixed)
    emit_literal(bw, ord('A'))
    emit_literal(bw, ord('B'))
    total = 2
    while total + 258 <= 32768:
        emit_match(bw, 258, 2)
        total += 258
    assert total == 32768, total
    emit_match(bw, 3, 32768)  # 合法：输出已有 32768 字节
    emit_eob(bw)
    plain = ab_pattern(32768) + b"ABA"
    return wrap_zlib(bw.bytes(), plain), plain


def handcrafted_bad_distance():
    """回溯越界：必须在**输出还没那么长**时就引用它。

    不能靠"距离 32769"来构造 —— DEFLATE 的距离空间止于 32768（码 29 的
    13 位额外位覆盖 24577..32768），32769 根本表示不出来，写下去会默默
    截成 24577，变成一个合法但错误的流。真正的越界是：只输出了 1 个字节，
    却引用距离 2。
    """
    bw = BitWriter()
    bw.raw(1, 1)
    bw.raw(1, 2)
    emit_literal(bw, ord('A'))
    emit_match(bw, 3, 2)  # 已输出 1 字节，距离 2 指向输出开始之前
    emit_eob(bw)
    return wrap_zlib(bw.bytes(), b"A")


def handcrafted_bad_length_symbol():
    """长度码 286/287 是保留值 —— 必须失败。"""
    bw = BitWriter()
    bw.raw(1, 1)
    bw.raw(1, 2)
    emit_literal(bw, ord('x'))
    c, n = lit_code(286)
    bw.code(c, n)
    emit_eob(bw)
    return wrap_zlib(bw.bytes(), b"x")


def handcrafted_reserved_block():
    """BTYPE = 11 是保留块类型。"""
    bw = BitWriter()
    bw.raw(1, 1)
    bw.raw(3, 2)
    return wrap_zlib(bw.bytes(), b"")


# ──────────────────────────────────────────────────────────────────────
# PNG 手写（chunk 写入 + 行滤波器编码）
# ──────────────────────────────────────────────────────────────────────

def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

def png_file(w, h, depth, ctype, raw_scanlines, plte=None, trns=None, interlace=0):
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    out += chunk(b"IDAT", zlib.compress(raw_scanlines, 6))
    out += chunk(b"IEND", b"")
    return out


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def encode_scanlines(rows, bpp, filt):
    """把未滤波的行按滤波器类型 `filt` 编码（filt 可以是 int 或 per-row 列表）。

    注意：这个函数**不**参与期望值的计算 —— 期望值直接来自 `rows`。
    否则编码器和解码器会共享同一个 bug，"通过"就变成了自我印证。
    """
    out = bytearray()
    prev = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        f = filt[y] if isinstance(filt, list) else filt
        out.append(f)
        if f == 0:
            enc = bytearray(row)
        elif f == 1:
            enc = bytearray((row[i] - (row[i - bpp] if i >= bpp else 0)) & 0xFF
                            for i in range(len(row)))
        elif f == 2:
            enc = bytearray((row[i] - prev[i]) & 0xFF for i in range(len(row)))
        elif f == 3:
            enc = bytearray((row[i] - (((row[i - bpp] if i >= bpp else 0) + prev[i]) // 2)) & 0xFF
                            for i in range(len(row)))
        elif f == 4:
            enc = bytearray()
            for i in range(len(row)):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                enc.append((row[i] - paeth(a, b, c)) & 0xFF)
        else:
            raise ValueError(f)
        out += enc
        prev = row
    return bytes(out)


def rows_of(pixels, channels, width):
    """扁平像素列表 → 行字节序列（8 位）。"""
    stride = width * channels
    return [bytes(pixels[y * stride:(y + 1) * stride])
            for y in range(len(pixels) // stride)]


# ──────────────────────────────────────────────────────────────────────
# 向量清单
# ──────────────────────────────────────────────────────────────────────

inflate_cases = []   # (name, zlib_stream, plain_expected)
inflate_bad = []     # (name, zlib_stream, err_substr)
png_cases = []       # (name, file, rgba_expected, w, h)
png_bad = []         # (name, file, err_substr)

def zlib_stream(data, level, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
    return c.compress(data) + c.flush()


# 长输出（~68 KB）：`pattern(4096)` 重复 17 次。它必须**可压缩** —— 用
# 不可压缩的伪随机数据当长向量，压缩流自身就有 70 KB，头文件白白胖胖
# （而且证明不了任何 inflate 行为：那只是 stored 块）。重复结构让 zlib
# 产出跨 32K 窗口的 LZ77 回溯 + 多块 dynamic 流，而明文由 C++ 用同一个
# 公式重新生成（pat=3），不入头文件。
REP_BLOCK, REP_TIMES = 4096, 17
rep_plain = pattern(REP_BLOCK) * REP_TIMES

# --- inflate：三种块类型，分别来自 zlib 的不同策略 ---
for name, level, strategy, data, pat in [
    ("empty/stored", 0, zlib.Z_DEFAULT_STRATEGY, b"", 0),
    ("single-byte/stored", 0, zlib.Z_DEFAULT_STRATEGY, b"X", 0),
    ("stored-300", 0, zlib.Z_DEFAULT_STRATEGY, bytes(range(256)) + b"stored-block-payload" * 2, 0),
    ("fixed-1k", 1, zlib.Z_FIXED, b"fox fixed-huffman " * 40, 0),
    ("fixed-500", 6, zlib.Z_FIXED, bytes([(i * 7 + i // 11) & 0xFF for i in range(500)]), 0),
    ("dynamic-1k", 9, zlib.Z_DEFAULT_STRATEGY,
     (b"the quick brown fox jumps over the lazy dog. " * 12 +
      bytes(range(64)) + b"tail"), 0),
    ("dynamic-run", 9, zlib.Z_DEFAULT_STRATEGY, b"\x00" * 4096 + b"end", 0),
    ("dynamic-mixed", 6, zlib.Z_DEFAULT_STRATEGY, bytes([i % 251 for i in range(2048)]), 0),
    ("repeat-4096x17", 9, zlib.Z_DEFAULT_STRATEGY, rep_plain, 3),
]:
    z = zlib_stream(data, level, strategy)
    assert len(z) < 8192, (name, len(z))  # 头文件里不该出现几十 KB 的流
    inflate_cases.append((name, z, data, pat))

# 手写：窗口边界、以及必须失败的流
wz, wp = handcrafted_window_boundary()
inflate_cases.append(("window-edge-32768", wz, wp, 2))
inflate_bad.append(("distance-32769", handcrafted_bad_distance(), "distance points before"))
inflate_bad.append(("length-symbol-286", handcrafted_bad_length_symbol(), "invalid length symbol"))
inflate_bad.append(("reserved-block-type-3", handcrafted_reserved_block(), "reserved deflate block type"))
inflate_bad.append(("bad-header", b"\x00\x01\x00\x00\x00\x00", "not deflate"))
inflate_bad.append(("adler-mismatch", (lambda z: z[:-1] + bytes([z[-1] ^ 0xFF]))(
    zlib.compress(b"adler is the second lock", 6)), "Adler-32 mismatch"))
inflate_bad.append(("truncated", zlib.compress(b"truncated stream body" * 8, 9)[:12], "truncated"))
inflate_bad.append(("too-short", b"\x78\x01\x03", "too short"))

# --- zlib_store（fox 自己的 stored 块写入器）对拍 zlib ---
# 这一组不是"解回来一样"，而是**字节完全一样**：`zlib.compress(data, 0)`
# 产出的就是 stored 块，而 fox 的 zlib_store() 声称写的是同一种流。
# 格式差一个字节（比如 FCHECK、LEN/NLEN 顺序）都会在这里现形。
store_refs = []  # (name, plain, zlib_level0)
for name, data in [("empty", b""), ("20-bytes", bytes(range(20))),
                   ("300-bytes", bytes([(i * 13 + 7) & 0xFF for i in range(300)]))]:
    store_refs.append((name, data, zlib.compress(data, 0)))

# --- PNG：每种行滤波器、每种颜色类型 ---
# 6x5 RGB：每行一种滤波器（0..4），像素值各不相同。
W, H = 6, 5
rgb_px = []
for y in range(H):
    for x in range(W):
        rgb_px += [(x * 40 + y * 3) & 0xFF, (y * 50 + x * 7) & 0xFF, (x * x * 5 + y * 11) & 0xFF]
rows = rows_of(rgb_px, 3, W)
expected_rgba = bytearray()
for p in range(W * H):
    expected_rgba += bytes(rgb_px[p * 3:p * 3 + 3]) + b"\xff"
png_cases.append(("rgb-filters-0-4",
                  png_file(W, H, 8, 2, encode_scanlines(rows, 3, [0, 1, 2, 3, 4])),
                  bytes(expected_rgba), W, H))

# 5x5 RGBA：全 Paeth —— 有 alpha 的渐变，符号错误在这里现形。
W, H = 5, 5
rgba_px = []
for y in range(H):
    for x in range(W):
        rgba_px += [(30 + x * 40) & 0xFF, (200 - y * 30) & 0xFF, (x * y * 10 + 5) & 0xFF,
                    (255 - (x + y) * 10) & 0xFF]
rows = rows_of(rgba_px, 4, W)
png_cases.append(("rgba-paeth", png_file(W, H, 8, 6, encode_scanlines(rows, 4, 4)),
                  bytes(rgba_px), W, H))

# 灰度 depth 4（子字节打包，MSB-first）
W, H = 7, 3
gray_vals = []
rows = []
for y in range(H):
    vals = [(y * 7 + x * 3) % 16 for x in range(W)]
    gray_vals += vals
    row = bytearray()
    for i in range(0, W, 2):
        hi = vals[i] & 0xF
        lo = vals[i + 1] & 0xF if i + 1 < W else 0
        row.append((hi << 4) | lo)
    rows.append(bytes(row))
exp = bytearray()
for v in gray_vals:
    g = v * 255 // 15
    exp += bytes([g, g, g, 255])
png_cases.append(("gray-4bit", png_file(W, H, 4, 0, encode_scanlines(rows, 1, 0)), bytes(exp), W, H))

# 灰度 depth 1（8 像素/字节）
W, H = 8, 2
bits = [1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1]
rows = []
for y in range(H):
    row = bytearray()
    for i in range(0, 8, 8):
        b = 0
        for j in range(8):
            b = (b << 1) | bits[y * 8 + i + j]
        row.append(b)
    rows.append(bytes(row))
exp = bytearray()
for v in bits:
    g = 255 if v else 0
    exp += bytes([g, g, g, 255])
png_cases.append(("gray-1bit", png_file(W, H, 1, 0, encode_scanlines(rows, 1, 0)), bytes(exp), W, H))

# 调色板 + tRNS（alpha 逐色）
W, H = 4, 2
plte = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])
trns = bytes([255, 128, 0, 255])
idx = [0, 1, 2, 3, 3, 2, 1, 0]
rows = encode_scanlines([bytes(idx[:4]), bytes(idx[4:])], 1, 0)
exp = bytearray()
for i in idx:
    exp += plte[i * 3:i * 3 + 3] + bytes([trns[i]])
png_cases.append(("palette-trns", png_file(W, H, 8, 3, rows, plte=plte, trns=trns), bytes(exp), W, H))

# 调色板 depth 4（打包索引）
W, H = 5, 2
plte2 = bytes([10, 20, 30, 200, 100, 50, 0, 0, 0])
idx2 = [(x + y * 3) % 3 for y in range(H) for x in range(W)]
rowbytes = []
for y in range(H):
    vals = idx2[y * W:(y + 1) * W]
    rb = bytearray()
    for i in range(0, W, 2):
        hi = vals[i] & 0xF
        lo = vals[i + 1] & 0xF if i + 1 < W else 0
        rb.append((hi << 4) | lo)
    rowbytes.append(bytes(rb))
exp = bytearray()
for i in idx2:
    exp += plte2[i * 3:i * 3 + 3] + b"\xff"
png_cases.append(("palette-4bit", png_file(W, H, 4, 3, encode_scanlines(rowbytes, 1, 0), plte=plte2),
                  bytes(exp), W, H))

# 灰度 + alpha
W, H = 3, 2
ga = [100, 255, 150, 128, 200, 0, 50, 64, 250, 200, 10, 255]
rows = encode_scanlines(rows_of(ga, 2, W), 2, 3)
exp = bytearray()
for p in range(W * H):
    g, a = ga[p * 2], ga[p * 2 + 1]
    exp += bytes([g, g, g, a])
png_cases.append(("gray-alpha", png_file(W, H, 8, 4, rows), bytes(exp), W, H))

# RGB depth 16 → 取高位
W, H = 3, 2
vals16 = [(0x1234, 0x5678, 0x9ABC), (0xFFFF, 0x0000, 0x8000), (0x00FF, 0xFF00, 0x0F0F),
          (0x1111, 0x2222, 0x3333), (0xABCD, 0xEF01, 0x2345), (0x9876, 0x5432, 0x10FE)]
rows_rgb16 = []
for y in range(H):
    rb = bytearray()
    for x in range(W):
        for v in vals16[y * W + x]:
            rb += struct.pack(">H", v)
    rows_rgb16.append(bytes(rb))
exp = bytearray()
for t in vals16:
    exp += bytes([(v >> 8) & 0xFF for v in t]) + b"\xff"
png_cases.append(("rgb-16bit", png_file(W, H, 16, 2, encode_scanlines(rows_rgb16, 6, 2)),
                  bytes(exp), W, H))

# tRNS 灰度键（值相等的像素变透明）
W, H = 4, 2
gvals = [10, 200, 10, 55, 77, 10, 200, 200]
rows = encode_scanlines([bytes(gvals[:4]), bytes(gvals[4:])], 1, 0)
exp = bytearray()
for v in gvals:
    exp += bytes([v, v, v, 0 if v == 200 else 255])
png_cases.append(("gray-trns-key", png_file(W, H, 8, 0, rows, trns=struct.pack(">H", 200)),
                  bytes(exp), W, H))

# 4x4 RGBA，2x2 色块 —— mipmap 测试的基准（每级都是精确平均）
W, H = 4, 4
blk = [(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 255), (255, 255, 0, 64)]
rows = []
for y in range(H):
    rb = bytearray()
    for x in range(W):
        c = blk[(y // 2) * 2 + (x // 2)]
        rb += bytes(c)
    rows.append(bytes(rb))
exp = bytearray()
for y in range(H):
    for x in range(W):
        exp += bytes(blk[(y // 2) * 2 + (x // 2)])
png_cases.append(("blocks-4x4-mip", png_file(W, H, 8, 6, encode_scanlines(rows, 4, 1)),
                  bytes(exp), W, H))

# --- PNG 负例：必须失败，而且要失败在**正确的**原因上 ---
good = png_file(2, 2, 8, 6, encode_scanlines([b"\x01\x02\x03\x04" * 1 + b"\x05\x06\x07\x08", b"\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10"], 4, 0))
png_bad.append(("bad-signature", b"\x89PNG\r\n\x1a\x0b" + good[8:], "bad signature"))
bad_crc = bytearray(good)
bad_crc[20] ^= 0xFF  # IHDR 数据里翻一位，CRC 不动
png_bad.append(("bad-crc", bytes(bad_crc), "CRC mismatch"))
png_bad.append(("truncated", good[:int(len(good) * 0.6)], "truncated"))
ihdr_only = b"\x89PNG\r\n\x1a\n" + good[8:33] + chunk(b"IEND", b"")
png_bad.append(("no-idat", ihdr_only, "no IDAT"))
inter = png_file(2, 2, 8, 6, encode_scanlines([b"\x00" * 8, b"\x00" * 8], 4, 0), interlace=1)
png_bad.append(("interlaced", inter, "interlace is not supported"))

# 滤波器字节 5：重新压一遍，CRC 是对的，错的是滤波器类型
rows = [b"\x01\x02\x03\x04", b"\x05\x06\x07\x08"]
bad_filt = bytearray()
bad_filt.append(5)
bad_filt += rows[0]
bad_filt.append(0)
bad_filt += rows[1]
png_bad.append(("bad-filter-5", png_file(1, 2, 8, 6, bytes(bad_filt)), "unknown PNG row filter"))

# zlib adler 错（chunk CRC 重新算过，所以错在 IDAT 内容本身）
zbody = zlib.compress(encode_scanlines(rows, 4, 0), 6)
zbody_bad = zbody[:-1] + bytes([zbody[-1] ^ 0xFF])
bad_png = (b"\x89PNG\r\n\x1a\n" + good[8:33] + chunk(b"IDAT", zbody_bad) + chunk(b"IEND", b""))
png_bad.append(("adler-in-idat", bad_png, "Adler-32 mismatch"))

# zlib 头错
bad_png2 = (b"\x89PNG\r\n\x1a\n" + good[8:33] + chunk(b"IDAT", b"\x00\x01\xff\xff") + chunk(b"IEND", b""))
png_bad.append(("bad-zlib-header", bad_png2, "not deflate"))

# 颜色类型 3 + 深度 16（规范里不存在）
ihdr_bad = struct.pack(">IIBBBBB", 2, 2, 16, 3, 0, 0, 0)
png_bad.append(("palette-16bit", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr_bad) +
                chunk(b"PLTE", bytes([1, 2, 3])) + chunk(b"IDAT", zlib.compress(b"\x00" * 4, 6)) +
                chunk(b"IEND", b""), "unsupported color type"))

# 调色板越界索引
plte3 = bytes([1, 2, 3, 4, 5, 6])
png_bad.append(("palette-index-oob", png_file(2, 1, 8, 3, encode_scanlines([bytes([0, 2])], 1, 0),
                                               plte=plte3), "palette index out of range"))

# ──────────────────────────────────────────────────────────────────────
# 输出 C++ 头文件
# ──────────────────────────────────────────────────────────────────────

def carr(name, data, per=12):
    lines = [f"inline const unsigned char {name}[] = {{"]
    for i in range(0, len(data), per):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in data[i:i + per]))
    lines.append("};")
    return "\n".join(lines)


out = []
out.append("// GENERATED by runtime/tests/gen_png_vectors.py - do not edit by hand.")
out.append("//")
out.append("// Regenerate:  python3 runtime/tests/gen_png_vectors.py > \\")
out.append("//                  runtime/tests/fox_png_vectors.hpp")
out.append("//")
out.append("// The zlib streams here were produced by CPython's zlib (i.e. by zlib"),
out.append("// itself, not by fox); the hand-crafted ones exist for the cases zlib will")
out.append("// never emit on purpose: the exact 32768 LZ77 window edge, and four invalid")
out.append("// streams that must FAIL rather than decode to garbage.")
out.append("#pragma once")
out.append("#include <cstddef>")
out.append("#include <cstdint>")
out.append("")
out.append("namespace png_vectors")
out.append("{")
out.append("struct Blob { const char *name; const unsigned char *data; size_t len; };")
out.append("// pattern: 0 = plain bytes are in this header; 1 = gen_pattern(plain.len);")
out.append("//          2 = ab_pattern(plain.len - 3) followed by \"ABA\";")
out.append("//          3 = gen_pattern(4096) repeated 17 times (68 KB of output).")
out.append("struct InflateCase { const char *name; Blob z; Blob plain; unsigned pattern; };")
out.append("struct PngCase { const char *name; Blob png; Blob rgba; unsigned w, h; };")
out.append("struct BadCase { const char *name; Blob blob; const char *err_substr; };")
out.append("")

for i, (name, z, plain, pat) in enumerate(inflate_cases):
    out.append(carr(f"kInf{i}z", z))
    if pat == 0:
        out.append(carr(f"kInf{i}p", plain))
out.append("")
out.append("inline const InflateCase kInflateCases[] = {")
for i, (name, z, plain, pat) in enumerate(inflate_cases):
    if pat == 0:
        plain_ref = f'{{"{name}", kInf{i}p, sizeof(kInf{i}p)}}'
    else:
        plain_ref = f'{{"{name}", nullptr, {len(plain)}}}'
    out.append(f'    {{"{name}", {{"{name}", kInf{i}z, sizeof(kInf{i}z)}}, '
               f'{plain_ref}, {pat}}},')
out.append("};")
out.append("inline constexpr int kInflateCount = (int)(sizeof(kInflateCases) / sizeof(kInflateCases[0]));")
out.append("")

for i, (name, z, sub) in enumerate(inflate_bad):
    out.append(carr(f"kInfBad{i}", z))
out.append("")
out.append("inline const BadCase kInflateBadCases[] = {")
for i, (name, z, sub) in enumerate(inflate_bad):
    out.append(f'    {{"{name}", {{"{name}", kInfBad{i}, sizeof(kInfBad{i})}}, "{sub}"}},')
out.append("};")
out.append("inline constexpr int kInflateBadCount = "
           "(int)(sizeof(kInflateBadCases) / sizeof(kInflateBadCases[0]));")
out.append("")

out.append("// fox 的 zlib_store() 必须与 zlib 的 level 0 输出逐字节一致。")
out.append("struct StoreCase { const char *name; Blob plain; Blob z; };")
for i, (name, plain, z) in enumerate(store_refs):
    out.append(carr(f"kStore{i}p", plain))
    out.append(carr(f"kStore{i}z", z))
out.append("inline const StoreCase kStoreCases[] = {")
for i, (name, plain, z) in enumerate(store_refs):
    out.append(f'    {{"{name}", {{"{name}", kStore{i}p, sizeof(kStore{i}p)}}, '
               f'{{"{name}", kStore{i}z, sizeof(kStore{i}z)}}}},')
out.append("};")
out.append("inline constexpr int kStoreCount = (int)(sizeof(kStoreCases) / sizeof(kStoreCases[0]));")
out.append("")

for i, (name, f, rgba, w, h) in enumerate(png_cases):
    out.append(carr(f"kPng{i}", f))
    out.append(carr(f"kPng{i}x", rgba))
    out.append(f"// {name}: {w}x{h}, expected RGBA {len(rgba)} bytes")
out.append("")
out.append("inline const PngCase kPngCases[] = {")
for i, (name, f, rgba, w, h) in enumerate(png_cases):
    out.append(f'    {{"{name}", {{"{name}", kPng{i}, sizeof(kPng{i})}}, '
               f'{{"{name}", kPng{i}x, sizeof(kPng{i}x)}}, {w}, {h}}},')
out.append("};")
out.append("inline constexpr int kPngCount = (int)(sizeof(kPngCases) / sizeof(kPngCases[0]));")
out.append("")

for i, (name, f, sub) in enumerate(png_bad):
    out.append(carr(f"kPngBad{i}", f))
out.append("")
out.append("inline const BadCase kPngBadCases[] = {")
for i, (name, f, sub) in enumerate(png_bad):
    out.append(f'    {{"{name}", {{"{name}", kPngBad{i}, sizeof(kPngBad{i})}}, "{sub}"}},')
out.append("};")
out.append("inline constexpr int kPngBadCount = "
           "(int)(sizeof(kPngBadCases) / sizeof(kPngBadCases[0]));")
out.append("")
out.append("} // namespace png_vectors")
out.append("")

sys.stdout.write("\n".join(out))
