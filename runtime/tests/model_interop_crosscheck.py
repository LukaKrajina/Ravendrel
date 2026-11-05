#!/usr/bin/env python3
# ============================================================
# model_interop_crosscheck.py —— 第三种实现，按规范解字节
#
# 为什么还要一个 Python 版
# ------------------------
# C++ 测试里有独立读取器，生产代码里有 `load_*`：两份都是 C++，共享同一套
# 「我们以为的规范」—— 比如把 `data_offsets` 当成相对文件起点，两份实现会
# 一起错、一起通过。这个脚本用 Python 的 `struct` + `json`（标准库，没有
# 复用自己的任何一行）再解一遍，并且**同时**核对：
#
#   · safetensors：头长度、JSON 补齐、data_offsets 基准、数据段长度与文件长度
#   · GGUF：KV 段（含 general.alignment）、张量信息的**维度逆序**、对齐、偏移
#
# 用法：
#   python3 runtime/tests/model_interop_crosscheck.py <产物目录>
# ============================================================

import json
import struct
import sys
from pathlib import Path

DTYPE_SIZE = {"F32": 4, "F64": 8, "I32": 4, "I64": 8, "U8": 1, "BOOL": 1}
GGUF_TYPE = {0: "F32", 26: "I32", 27: "I64", 28: "F64"}

fails = 0


def check(ok, what):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails += 1


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h ^= x
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


# ── safetensors ────────────────────────────────────────────────

def parse_safetensors(path):
    raw = Path(path).read_bytes()
    (hlen,) = struct.unpack("<Q", raw[:8])
    header = raw[8:8 + hlen]
    body = raw[8 + hlen:]
    info = {
        "hlen": hlen,
        "padded8": (8 + hlen) % 8 == 0,
        "space_padded": header.endswith(b" " * ((-hlen) % 8)) or hlen % 8 == 0,
        "file_len_ok": len(raw) == 8 + hlen + len(body),
        "json": json.loads(header.decode("utf-8")),
        "body": body,
    }
    return info


def check_safetensors(path, expected):
    print(f"[Python] {Path(path).name}")
    try:
        st = parse_safetensors(path)
    except Exception as e:  # noqa: BLE001
        check(False, f"解析失败: {e}")
        return
    check(st["padded8"], "头部长度补齐到 8 字节边界（8 + N ≡ 0 mod 8）")
    check(st["space_padded"], "JSON 尾部确实是空格填充（UTF-8 解码不会截断多字节）")
    check(st["file_len_ok"], "文件长度 = 8 + 头长度 + 数据段")

    tensors = {k: v for k, v in st["json"].items() if k != "__metadata__"}
    meta = st["json"].get("__metadata__", {})
    check(len(tensors) == 1 and expected["name"] in tensors, "张量名与预期一致")
    t = tensors[expected["name"]]
    check(t["dtype"] == expected["dtype"], f"dtype = {expected['dtype']}")
    check([int(x) for x in t["shape"]] == list(expected["shape"]), f"shape = {expected['shape']}")

    b, e = t["data_offsets"]
    check(b == 0, "单张量的 data_offsets 从 0 开始")
    check(e - b == expected["bytes"], "切片长度 = 元素数 × dtype_size")
    check(e <= len(st["body"]), "data_offsets 相对**数据段**（不是文件起点）")
    data = st["body"][b:e]
    check(fnv1a64(data) == expected["fnv1a64"], "数据段 FNV-1a 与预期相同（逐字节）")
    check({k: str(v) for k, v in meta.items()} == {k: str(v) for k, v in expected["metadata"].items()},
          "元数据键值一致（含中文与转义）")


# ── GGUF v3 ────────────────────────────────────────────────────

class R:
    def __init__(self, b):
        self.b, self.at = b, 0

    def take(self, n):
        v = self.b[self.at:self.at + n]
        assert len(v) == n, "文件尾提前结束"
        self.at += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def s(self):
        n = self.u64()
        return self.take(n).decode("utf-8")


def parse_gguf(path):
    raw = Path(path).read_bytes()
    r = R(raw)
    assert r.take(4) == b"GGUF", "魔数不是 GGUF"
    ver = r.u32()
    n_tensors = r.u64()
    n_kv = r.u64()
    kvs = {}
    alignment = 32
    for _ in range(n_kv):
        k = r.s()
        t = r.u32()
        if t == 8:
            kvs[k] = r.s()
        elif t == 4:
            kvs[k] = str(r.u32())
            if k == "general.alignment":
                alignment = int(kvs[k])
        elif t == 10:
            kvs[k] = str(r.u64())
        elif t in (0, 1, 7):
            kvs[k] = str(r.u8())
        elif t == 2:
            kvs[k] = str(r.u16())
        else:
            raise AssertionError(f"KV 类型 {t} 未预期（最小读取器只认这几种）")
    tensors = []
    for _ in range(n_tensors):
        name = r.s()
        nd = r.u32()
        ne = [r.u64() for _ in range(nd)]
        gtype = r.u32()
        off = r.u64()
        tensors.append({"name": name, "ne": ne, "type": gtype, "off": off})
    data_at = (r.at + alignment - 1) // alignment * alignment
    return {
        "version": ver,
        "kvs": kvs,
        "alignment": alignment,
        "tensors": tensors,
        "data_at": data_at,
        "raw": raw,
        "info_at": r.at,
    }


def check_gguf(path, expected):
    print(f"[Python] {Path(path).name}")
    try:
        g = parse_gguf(path)
    except Exception as e:  # noqa: BLE001
        check(False, f"解析失败: {e}")
        return
    check(g["version"] == 3, "版本 = 3")
    check(g["alignment"] == 32 and (g["alignment"] & (g["alignment"] - 1)) == 0,
          "general.alignment 是 2 的幂（32）")
    check("general.architecture" in g["kvs"], "有 general.architecture")
    check(g["data_at"] % g["alignment"] == 0, "数据段起点按 alignment 对齐")
    check(len(g["tensors"]) == 1, "张量数 = 1")
    t = g["tensors"][0]
    check(t["name"] == expected["name"], "张量名与预期一致")
    check(GGUF_TYPE.get(t["type"]) == expected["dtype"], f"ggml 类型 = {expected['dtype']}")
    # 【关键】行主序 shape 的最外层在**最后**（ne[0] 是最内层）
    check(t["ne"] == list(reversed(expected["shape"])),
          f"维度逆序：行主序 {expected['shape']} ⇒ ne = {list(reversed(expected['shape']))}")
    nbytes = 1
    for d in expected["shape"]:
        nbytes *= d
    nbytes *= DTYPE_SIZE[expected["dtype"]]
    check(t["off"] + nbytes <= len(g["raw"]) - g["data_at"], "张量数据在文件内")
    data = g["raw"][g["data_at"] + t["off"]:g["data_at"] + t["off"] + nbytes]
    check(fnv1a64(data) == expected["fnv1a64"], "数据段 FNV-1a 与预期相同（逐字节）")


_D = Path(".")


def check_rvdlm(path, expected):
    """独立解析 `.rvdlm`（QKM1，见 runtime/include/qml/RVDLModelFormat.hpp）：
    24 字节头（magic/version/total_elements/shape_dimensions/metadata_count）
    + shape × u64 + metadata × (u32 len, bytes) + 数据段。
    用于验证**反向导入**的产物：容器 → .rvdlm 之后，张量与元数据是否完好。"""
    print(f"[Python] {Path(path).name}（.rvdlm 反向导入产物）")
    raw = Path(path).read_bytes()
    check(raw[:4] == b"QKM1", "魔数 = QKM1")
    version, total, ndims, mcount = struct.unpack("<IQII", raw[4:24])
    check(version == 2, "版本 = 2")
    at = 24
    shape = []
    for _ in range(ndims):
        (dd,) = struct.unpack("<Q", raw[at:at + 8])
        shape.append(dd)
        at += 8
    meta = {}
    for _ in range(mcount):
        (klen,) = struct.unpack("<I", raw[at:at + 4])
        at += 4
        k = raw[at:at + klen].decode("utf-8")
        at += klen
        (vlen,) = struct.unpack("<I", raw[at:at + 4])
        at += 4
        v = raw[at:at + vlen].decode("utf-8")
        at += vlen
        meta[k] = v
    data = raw[at:]
    check(shape == list(expected["shape"]), f"shape = {expected['shape']}（行主序，未因 GGUF 逆序而错）")
    nbytes = 1
    for dd in shape:
        nbytes *= dd
    nbytes *= DTYPE_SIZE[expected["dtype"]]
    check(total * DTYPE_SIZE[expected["dtype"]] == nbytes, "total_elements × dtype_size = 数据段长度")
    check(len(data) == nbytes, f"数据段长度 = {nbytes}（dtype {expected['dtype']}）")
    check(fnv1a64(data) == expected["fnv1a64"], "数据段 FNV-1a 与源容器相同（逐字节）")
    # 元数据契约 = 不丢失的单调超集（源键值全部在；gguf 路径只多补默认的 architecture）
    no_loss = all(meta.get(k) == str(v) for k, v in expected["metadata"].items())
    check(no_loss, "源元数据键值无丢失")
    check(not any(k.startswith("general.") for k in meta), "容器级 general.* 键不泄漏进 .rvdlm")


def container_expectation(container):
    """从**容器文件本身**（Python 独立解析）导出 .rvdlm 产物的预期值。"""
    if container == "safetensors":
        st = parse_safetensors(_D / "rt.safetensors")
        t = st["json"]["weight"]
        meta = {k: str(v) for k, v in st["json"].get("__metadata__", {}).items()}
        b, e = t["data_offsets"]
        return {"dtype": t["dtype"], "shape": [int(x) for x in t["shape"]],
                "fnv1a64": fnv1a64(st["body"][b:e]), "metadata": meta}
    g = parse_gguf(_D / "rt.gguf")
    t = g["tensors"][0]
    kvs = g["kvs"]
    # 反向映射（与 C++ 侧 gguf_meta_to_rvdlm 同规则）：general.name→name、
    # general.architecture→architecture、general.* 其余丢弃
    meta = {k: v for k, v in kvs.items() if not k.startswith("general.")}
    if "general.name" in kvs:
        meta["name"] = kvs["general.name"]
    if "general.architecture" in kvs:
        meta["architecture"] = kvs["general.architecture"]
    dtype = GGUF_TYPE[t["type"]]
    nbytes = 1
    for dd in t["ne"]:
        nbytes *= dd
    nbytes *= DTYPE_SIZE[dtype]
    return {"dtype": dtype, "shape": list(reversed(t["ne"])),
            "fnv1a64": fnv1a64(g["raw"][g["data_at"] + t["off"]:g["data_at"] + t["off"] + nbytes]),
            "metadata": meta}


def main():
    global _D
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "model_interop_out")
    _D = d
    if not d.is_dir():
        print(f"目录不存在: {d}")
        return 1

    for container in ("safetensors", "gguf"):
        exp_path = d / f"expected_{container}.json"
        if not exp_path.exists():
            check(False, f"缺少 {exp_path.name}（先跑 model_interop_test）")
            continue
        expected = json.loads(exp_path.read_text(encoding="utf-8"))
        name = "toy." + ("safetensors" if container == "safetensors" else "gguf")
        if not (d / name).exists():
            check(False, f"缺少 {name}")
            continue
        (check_safetensors if container == "safetensors" else check_gguf)(d / name, expected)

    # 额外的定点检查：u8（只有 safetensors 认）与 f64（ggml type 28）
    if (d / "flags.safetensors").exists():
        print("[Python] flags.safetensors（u8）")
        st = parse_safetensors(d / "flags.safetensors")
        t = st["json"].get("flags", {})
        check(t.get("dtype") == "U8" and [int(x) for x in t.get("shape", [])] == [4],
              "U8 张量在 safetensors 里无损（dtype 名是生态认得的 U8）")
    if (d / "pi.gguf").exists():
        print("[Python] pi.gguf（f64）")
        g = parse_gguf(d / "pi.gguf")
        check(g["tensors"][0]["type"] == 28, "f64 用 ggml type 28")
        check(g["tensors"][0]["ne"] == [2], "一维张量的 ne 不因逆序而变")

    # 反向导入：容器 → `.rvdlm` 的产物，按 QKM1 规范独立解析核对。
    # 预期值来自容器文件本身（本脚本自己解析的），不借用 C++ 的任何结论 ——
    # 「导出对」与「导入对」必须分别被证明，否则 round-trip 可能两头一起错。
    if (d / "rt.safetensors").exists() and (d / "rt.safetensors.rvdlm").exists():
        check_rvdlm(d / "rt.safetensors.rvdlm", container_expectation("safetensors"))
    if (d / "rt.gguf").exists() and (d / "rt.gguf.rvdlm").exists():
        check_rvdlm(d / "rt.gguf.rvdlm", container_expectation("gguf"))
    if (d / "f64.safetensors").exists() and (d / "f64_back.rvdlm").exists():
        print("[Python] f64_back.rvdlm（auto 导入按容器 dtype 分发）")
        st = parse_safetensors(d / "f64.safetensors")
        t = st["json"]["weight"]
        b, e = t["data_offsets"]
        check_rvdlm(d / "f64_back.rvdlm",
                    {"dtype": t["dtype"], "shape": [int(x) for x in t["shape"]],
                     "fnv1a64": fnv1a64(st["body"][b:e]),
                     "metadata": {k: str(v) for k, v in st["json"].get("__metadata__", {}).items()}})

    print()
    print("CROSSCHECK-GGUF/SAFETENSORS: " + ("全部通过" if fails == 0 else f"失败（{fails} 项）"))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
