#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 Unicode Unihan 数据库生成 vendor/pinyin/unihan_pinyin.hpp（汉字 → 拼音静态表）。

用法：
    python gen_unihan_pinyin.py [Unihan_Readings.txt 路径] [输出头文件路径]

- 若提供 Unihan_Readings.txt：解析其中的 kMandarin（普通话读音）字段，
  生成「全量」拼音表（覆盖所有含 kMandarin 的 CJK 表意文字）。
- 若未提供：使用脚本内置的「常用汉字拼音表」作为 fallback（日常高频字）。

数据来源：Unicode Unihan 数据库（Unicode License v3）
  下载：https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip
        （解压后取 Unihan_Readings.txt）
"""

import sys

# ════════════════════════════════════════════════════════════════
# 内置 fallback：常用汉字 → 拼音（无调）。未提供 Unihan 数据时使用。
# ════════════════════════════════════════════════════════════════
FALLBACK_PINYIN = {
    0x6211: "wo", 0x4F60: "ni", 0x4ED6: "ta", 0x5979: "ta", 0x5B83: "ta",
    0x7684: "de", 0x662F: "shi", 0x5728: "zai", 0x6709: "you", 0x548C: "he",
    0x4E86: "le", 0x4E0D: "bu", 0x4EBA: "ren", 0x8FD9: "zhe", 0x4E2D: "zhong",
    0x5927: "da", 0x4E3A: "wei", 0x4E0A: "shang", 0x4E2A: "ge", 0x56FD: "guo",
    0x7231: "ai", 0x597D: "hao", 0x5929: "tian", 0x5730: "di", 0x6C34: "shui",
    0x706B: "huo", 0x6728: "mu", 0x91D1: "jin", 0x571F: "tu", 0x65E5: "ri",
    0x6708: "yue", 0x661F: "xing", 0x5FC3: "xin", 0x624B: "shou", 0x53E3: "kou",
    0x76EE: "mu", 0x8033: "er", 0x5C71: "shan", 0x5DDD: "chuan", 0x82B1: "hua",
    0x8349: "cao", 0x6811: "shu", 0x9E1F: "niao", 0x9C7C: "yu", 0x866B: "chong",
    0x9A6C: "ma", 0x725B: "niu", 0x7F8A: "yang", 0x732B: "mao", 0x72D7: "gou",
    0x9F99: "long", 0x51E4: "feng",
    0x4E00: "yi", 0x4E8C: "er", 0x4E09: "san", 0x56DB: "si", 0x4E94: "wu",
    0x516D: "liu", 0x4E03: "qi", 0x516B: "ba", 0x4E5D: "jiu", 0x5341: "shi",
    0x767E: "bai", 0x5343: "qian", 0x4E07: "wan", 0x4EBF: "yi", 0x96F6: "ling",
    0x4E1C: "dong", 0x5357: "nan", 0x897F: "xi", 0x5317: "bei",
    0x6625: "chun", 0x590F: "xia", 0x79CB: "qiu", 0x51AC: "dong",
    0x98CE: "feng", 0x96E8: "yu", 0x96F7: "lei", 0x7535: "dian", 0x4E91: "yun",
    0x96EA: "xue", 0x96FE: "wu", 0x9732: "lu", 0x971C: "shuang",
    0x65F6: "shi", 0x7A7A: "kong", 0x5149: "guang", 0x97F3: "yin", 0x8272: "se",
    0x751F: "sheng", 0x6B7B: "si", 0x547D: "ming", 0x9053: "dao", 0x6CD5: "fa",
    0x7406: "li", 0x6C14: "qi", 0x795E: "shen", 0x4ED9: "xian", 0x4F5B: "fo",
    0x9B54: "mo", 0x9B3C: "gui", 0x5996: "yao", 0x7075: "ling",
    0x91CF: "liang", 0x5B50: "zi", 0x71B5: "shang", 0x6CE2: "bo", 0x7C92: "li",
    0x573A: "chang", 0x529B: "li", 0x80FD: "neng", 0x529F: "gong", 0x52BF: "shi",
    0x94A5: "yao", 0x5319: "chi", 0x5BC6: "mi", 0x7801: "ma", 0x7B7E: "qian",
    0x540D: "ming", 0x5E01: "bi", 0x94FE: "lian", 0x5757: "kuai", 0x8D26: "zhang",
    0x672C: "ben", 0x533A: "qu", 0x7ED3: "jie", 0x70B9: "dian", 0x7F51: "wang",
    0x7EDC: "luo", 0x670D: "fu", 0x52A1: "wu", 0x534F: "xie", 0x8BAE: "yi",
    0x5171: "gong", 0x8BC6: "shi", 0x6316: "wa", 0x77FF: "kuang",
    0x4EA4: "jiao", 0x6613: "yi", 0x8F6C: "zhuan", 0x4F59: "yu", 0x989D: "e",
    0x94B1: "qian", 0x503C: "zhi", 0x4EF7: "jia", 0x6570: "shu", 0x5B57: "zi",
    0x9A8C: "yan", 0x8BC1: "zheng", 0x53D1: "fa", 0x884C: "xing",
    0x94F8: "zhu", 0x9020: "zao", 0x9500: "xiao", 0x6BC1: "hui",
    0x589E: "zeng", 0x51CF: "jian", 0x4E58: "cheng", 0x9664: "chu",
    0x4E16: "shi", 0x754C: "jie", 0x5E73: "ping", 0x672A: "wei", 0x6765: "lai",
    0x8FC7: "guo", 0x53BB: "qu", 0x73B0: "xian", 0x6C38: "yong", 0x6052: "heng",
    0x65E0: "wu", 0x7A77: "qiong", 0x9650: "xian", 0x8D85: "chao", 0x8D8A: "yue",
    0x56E0: "yin", 0x679C: "guo", 0x5F8B: "lv", 0x66F2: "qu", 0x7387: "lv",
    0x5F15: "yin", 0x5FEB: "kuai", 0x5E7D: "you", 0x7EA0: "jiu", 0x7F20: "chan",
    0x9690: "yin", 0x5F62: "xing", 0x4F20: "chuan", 0x9001: "song",
    0x6298: "zhe", 0x53E0: "die", 0x5B87: "yu", 0x5B99: "zhou",
    0x94F6: "yin", 0x6CB3: "he", 0x592A: "tai", 0x9633: "yang", 0x7403: "qiu",
}

# 声调 → 无调映射（Unihan kMandarin 带声调，需去调以匹配无调拼音）
TONE_STRIP = str.maketrans(
    "āáǎàēéěèīíǐìōóǒòūúǔùǖǘǚǜüńňǹḿ",
    "aaaaeeeeiiiioooouuuuvvvvvnnnm",
)


def strip_tone(s: str) -> str:
    return s.translate(TONE_STRIP)


def parse_unihan(path: str) -> dict:
    """解析 Unihan_Readings.txt，返回 {码点: 无调拼音}。"""
    entries = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("U+"):
                continue
            if "kMandarin" not in line:
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            cp = int(parts[0][2:], 16)
            pinyin = strip_tone(parts[2].strip())
            if pinyin:
                entries[cp] = pinyin
    return entries


def emit_header(entries: dict, out_path: str) -> None:
    items = sorted(entries.items())
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n")
        f.write("// 由 scripts/gen_unihan_pinyin.py 自动生成，请勿手改。\n")
        f.write("// 数据来源：Unicode Unihan 数据库 kMandarin 字段（Unicode License v3）\n")
        f.write("// 内容：汉字 Unicode 码点 → 无调拼音（如「我」0x6211 → \"wo\"）。\n")
        f.write("#include <cstdint>\n\n")
        f.write("namespace qhal::pinyin\n{\n")
        f.write("    // 按码点升序排列，供二分查找。\n")
        f.write("    struct Entry\n")
        f.write("    {\n")
        f.write("        uint32_t codepoint;\n")
        f.write("        const char *pinyin;\n")
        f.write("    };\n\n")
        f.write("    static constexpr Entry kTable[] = {\n")
        for cp, py in items:
            f.write('        {0x%04X, "%s"},\n' % (cp, py))
        f.write("    };\n")
        f.write("    static constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);\n")
        f.write("} // namespace qhal::pinyin\n")
    print(f"[gen_unihan_pinyin] wrote {len(items)} entries -> {out_path}")


def main() -> None:
    args = sys.argv[1:]
    out_path = "vendor/pinyin/unihan_pinyin.hpp"
    entries = None

    if args and args[0] and args[0] != "-":
        unihan_path = args[0]
        print(f"[gen_unihan_pinyin] parsing {unihan_path} ...")
        entries = parse_unihan(unihan_path)
        if len(args) >= 2:
            out_path = args[1]
    else:
        print("[gen_unihan_pinyin] no Unihan data provided, using builtin fallback table.")
        entries = dict(FALLBACK_PINYIN)

    if not entries:
        print("[gen_unihan_pinyin] ERROR: empty table.")
        sys.exit(1)

    emit_header(entries, out_path)


if __name__ == "__main__":
    main()
