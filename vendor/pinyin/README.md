# vendor/pinyin —— 汉字拼音数据（Unihan）

本目录提供「汉字 → 无调拼音」的静态数据表，供 `qhal::UnicodeHash256`（语义哈希）使用。

## 文件

| 文件 | 说明 |
| --- | --- |
| `unihan_pinyin.hpp` | 自动生成的拼音表（约 4.4 万条，覆盖所有含 `kMandarin` 的 CJK 汉字） |
| `LICENSE` | Unicode License v3 |

## 数据来源

- **Unicode Unihan 数据库**（`Unihan_Readings.txt` 的 `kMandarin` 字段）
- 下载：<https://www.unicode.org/Public/UCD/latest/ucd/Unihan.zip>
- 许可证：[Unicode License v3](https://www.unicode.org/license.txt)

## 再生成方法

```bash
# 1. 下载并解压 Unihan 数据库，取其中的 Unihan_Readings.txt
# 2. 运行生成脚本
py scripts/gen_unihan_pinyin.py <Unihan_Readings.txt 路径> vendor/pinyin/unihan_pinyin.hpp
```

> 生成脚本会将带声调的拼音（如 `wǒ`）去调为无调拼音（`wo`），与 `UnicodeHash256` 的
> 「汉字 → 英文」语义归一化一致（「我」与「wo」哈希相同）。
