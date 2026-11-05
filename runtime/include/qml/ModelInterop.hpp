#pragma once
// ============================================================
// ModelInterop.hpp —— `.rvdlm` → safetensors / gguf 的双向容器转换
//
// 为什么需要它
// ------------
// `.rvdlm`（qml::ModelExporter，魔数 `QKM1`）是这个项目自己的格式：单张量 +
// 一段任意键值元数据。它够用，但**外界读不懂** —— 想拿权重去微调、去可视化、
// 去和别人的实现比对，都必须先落到生态认得的容器里。这里做两个：
//
//   safetensors：8 字节小端头长度 + JSON + 数据段。JSON 用空格补齐到 8 字节。
//                `dtype`/`shape`/`data_offsets` 三个键是全部语义。
//   GGUF v3    ：`GGUF` + 版本 + 张量数 + KV 数 → KV 段 → 张量信息段 → 对齐后的
//                数据段。**维度是逆序的**（`ne[0]` 是最内层），这是这份代码里
//                最容易写错、也是唯一一个"写反了还能读回来"的坑。
//
// 六条设计约定（每条都是踩过或差点踩到的）
// ----------------------------------------
// 1. **`.rvdlm` 不记录元素类型**。QKMHeader 里没有 dtype 字段，所以**导出**方必须
//    **声明**类型。这不是可以猜的东西：把 f64 的 8 字节按 f32 重组，得到的是一份
//    "看起来像张量"的垃圾，而且没有任何一步会报错。导出的入口因此是一个
//    模板参数（`export_rvdlm_to_*(...， T)`），而不是运行时推断。
//    **导入方向相反**：容器自带 dtype（safetensors JSON / GGUF 张量信息段），
//    故 `import_*_to_rvdlm_auto` 按容器 dtype 分发，不要求调用方声明；
//    手动指定 T 的入口保留，但先与容器 dtype 对账，不一致即报错。
// 2. **写出必须逐字节确定**。元数据用有序容器（`Metadata` = vector<pair>），
//    导出前排序 —— unordered_map 的迭代顺序会让同一份权重导出两次得到两个
//    不同的文件，而文件是要被 diff / 哈希 / 进版本库的。
// 3. **类型映射宁缺毋猜**。safetensors 认全部六种（F32/F64/I32/I64/U8/BOOL）；
//    GGUF 的 ggml 类型表里**没有** u8/bool，所以导出 u8/bool 到 gguf 是
//    **报错**，不是悄悄升成 I32。（读回来的时候也一样：不认识的 ggml 类型报错，
//    因为量化类型（Q4_0…）读进来也不能用，静态假装支持比不支持更坏。）
// 4. **字节序显式写出**，不 memcpy 结构体。safetensors/GGUF 都定义在小端上，
//    而结构体的 padding/对齐是编译器说了算的。数据段本身按主机序 memcpy，
//    因此这份实现只在小端主机上逐字节正确 —— 这是**已知限制**，不假装解决。
// 5. **失败返回错误串，不抛异常**：容器是 I/O 边界，调用方（stdin 命令、测试）
//    需要能把"哪个字段不对"直接打出来。
// 6. **`load_*` 做完整边界校验**：偏移越界、切片长度与 shape×dtype 不符、
//    对齐不是 2 的幂、维度数为 0 —— 全部报错。读到一半才发现的问题比当场拒绝
//    贵得多。
//
// 双向：`save_*` / `load_*`（容器级）与 `export_rvdlm_to_*` / `import_*_to_rvdlm`
// （`.rvdlm` 端到端）都在这里（header-only，无第三方依赖）。
// 测试里另有一份**独立的最小读取器**（不调用本文件任何代码）反向解析校验字节
// 布局，还有一份 Python 脚本做跨语言交叉验证 —— 自己读自己写的东西只能证明
// "自洽"，不能证明"符合规范"。
// ============================================================

#include "RVDLModelFormat.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace qml
{
    namespace interop
    {
        using u8 = std::uint8_t;
        using u32 = std::uint32_t;
        using u64 = std::uint64_t;

        // ── 类型 ─────────────────────────────────────────────────────────

        enum class DType : u8
        {
            F32,
            F64,
            I32,
            I64,
            U8,
            BOOL,
        };

        inline const char *dtype_name(DType t)
        {
            switch (t)
            {
            case DType::F32: return "F32";
            case DType::F64: return "F64";
            case DType::I32: return "I32";
            case DType::I64: return "I64";
            case DType::U8: return "U8";
            case DType::BOOL: return "BOOL";
            }
            return "?";
        }

        inline u32 dtype_size(DType t)
        {
            switch (t)
            {
            case DType::F32: return 4;
            case DType::F64: return 8;
            case DType::I32: return 4;
            case DType::I64: return 8;
            case DType::U8: return 1;
            case DType::BOOL: return 1;
            }
            return 0;
        }

        inline bool dtype_from_name(const std::string &s, DType &out)
        {
            if (s == "F32") out = DType::F32;
            else if (s == "F64") out = DType::F64;
            else if (s == "I32") out = DType::I32;
            else if (s == "I64") out = DType::I64;
            else if (s == "U8") out = DType::U8;
            else if (s == "BOOL") out = DType::BOOL;
            else return false;
            return true;
        }

        /// ggml 类型号 ↔ DType。
        ///
        /// 只映射**确实存在且无损**的四种。ggml 表里没有 u8/bool，量化类型
        /// （Q4_0 等）的字节布局也不是本文件的事 —— 两处都返回 false，由调用方
        /// 报错。`gguf_type_of(DType::U8)` 返回 false 是**答案**，不是缺陷。
        inline bool gguf_type_of(DType t, u32 &out)
        {
            switch (t)
            {
            case DType::F32: out = 0; return true;   // GGML_TYPE_F32
            case DType::F64: out = 28; return true;  // GGML_TYPE_F64
            case DType::I32: out = 26; return true;  // GGML_TYPE_I32
            case DType::I64: out = 27; return true;  // GGML_TYPE_I64
            default: return false;
            }
        }

        inline bool dtype_of_gguf(u32 t, DType &out)
        {
            switch (t)
            {
            case 0: out = DType::F32; return true;
            case 28: out = DType::F64; return true;
            case 26: out = DType::I32; return true;
            case 27: out = DType::I64; return true;
            default: return false;
            }
        }

        /// C++ 类型 → 容器类型。未实现的类型在**编译期**就拒绝。
        template <typename T>
        struct dtype_of;

        template <> struct dtype_of<float> { static constexpr DType value = DType::F32; };
        template <> struct dtype_of<double> { static constexpr DType value = DType::F64; };
        template <> struct dtype_of<std::int32_t> { static constexpr DType value = DType::I32; };
        template <> struct dtype_of<std::int64_t> { static constexpr DType value = DType::I64; };
        template <> struct dtype_of<u8> { static constexpr DType value = DType::U8; };
        template <> struct dtype_of<bool> { static constexpr DType value = DType::BOOL; };

        // ── 数据 ─────────────────────────────────────────────────────────

        /// 一张张量 + 它的容器表示。
        struct TensorData
        {
            std::string name = "weight";
            DType dtype = DType::F32;
            std::vector<u64> shape;      ///< 行主序（最外层在前），与 `.rvdlm` 一致
            std::vector<u8> bytes;       ///< 原始字节，长度 = 元素数 × dtype_size
        };

        /// 有序元数据。**顺序有意义**：导出必须逐字节确定（见文件头第 2 条）。
        using Metadata = std::vector<std::pair<std::string, std::string>>;

        inline Metadata sorted_metadata(const std::unordered_map<std::string, std::string> &m)
        {
            Metadata out(m.begin(), m.end());
            std::sort(out.begin(), out.end(),
                      [](const std::pair<std::string, std::string> &a,
                         const std::pair<std::string, std::string> &b) { return a.first < b.first; });
            return out;
        }

        namespace detail
        {
            // ── 小端字节写入 ─────────────────────────────────────────────
            inline void put_u16(std::vector<u8> &v, std::uint16_t x)
            {
                v.push_back((u8)(x & 0xFF));
                v.push_back((u8)((x >> 8) & 0xFF));
            }
            inline void put_u32(std::vector<u8> &v, u32 x)
            {
                for (int i = 0; i < 4; ++i) v.push_back((u8)((x >> (8 * i)) & 0xFF));
            }
            inline void put_u64(std::vector<u8> &v, u64 x)
            {
                for (int i = 0; i < 8; ++i) v.push_back((u8)((x >> (8 * i)) & 0xFF));
            }
            inline void put_bytes(std::vector<u8> &v, const void *p, size_t n)
            {
                if (n == 0) return;
                const u8 *b = (const u8 *)p;
                v.insert(v.end(), b, b + n);
            }
            inline void put_str(std::vector<u8> &v, const std::string &s)
            {
                put_bytes(v, s.data(), s.size());
            }

            inline void patch_u64(std::vector<u8> &v, size_t at, u64 x)
            {
                for (int i = 0; i < 8; ++i) v[at + i] = (u8)((x >> (8 * i)) & 0xFF);
            }

            // ── 小端字节读取 ─────────────────────────────────────────────
            struct Reader
            {
                const u8 *p = nullptr;
                size_t n = 0;
                size_t at = 0;
                std::string err;

                bool need(size_t k)
                {
                    if (at + k <= n) return true;
                    if (err.empty()) err = "意外的文件结尾（偏移 " + std::to_string(at) + "）";
                    return false;
                }
                bool ru16(std::uint16_t &x)
                {
                    if (!need(2)) return false;
                    x = (std::uint16_t)(p[at] | (p[at + 1] << 8));
                    at += 2;
                    return true;
                }
                bool ru32(std::uint32_t &x)
                {
                    if (!need(4)) return false;
                    x = 0;
                    for (int i = 0; i < 4; ++i) x |= (u32)p[at + i] << (8 * i);
                    at += 4;
                    return true;
                }
                bool ru64(std::uint64_t &x)
                {
                    if (!need(8)) return false;
                    x = 0;
                    for (int i = 0; i < 8; ++i) x |= (u64)p[at + i] << (8 * i);
                    at += 8;
                    return true;
                }
                bool bytes(void *dst, size_t k)
                {
                    if (!need(k)) return false;
                    std::memcpy(dst, p + at, k);
                    at += k;
                    return true;
                }
                bool str(std::string &s, u64 len)
                {
                    if (len > (u64)1 << 32) { if (err.empty()) err = "字符串长度不合理"; return false; }
                    if (!need((size_t)len)) return false;
                    s.assign((const char *)p + at, (size_t)len);
                    at += (size_t)len;
                    return true;
                }
                bool gguf_str(std::string &s)
                {
                    u64 len = 0;
                    if (!ru64(len)) return false;
                    return str(s, len);
                }
            };

            inline std::vector<u8> read_file(const std::string &path, std::string *err)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    if (err) *err = "无法打开文件: " + path;
                    return {};
                }
                in.seekg(0, std::ios::end);
                const std::streamoff sz = in.tellg();
                in.seekg(0, std::ios::beg);
                if (sz < 0)
                {
                    if (err) *err = "无法确定文件长度: " + path;
                    return {};
                }
                std::vector<u8> out((size_t)sz);
                if (sz > 0) in.read((char *)out.data(), sz);
                if (!in && !in.eof())
                {
                    if (err) *err = "读取文件失败: " + path;
                    return {};
                }
                return out;
            }

            inline bool write_file(const std::string &path, const std::vector<u8> &v, std::string *err)
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    if (err) *err = "无法写入文件: " + path;
                    return false;
                }
                if (!v.empty()) out.write((const char *)v.data(), (std::streamsize)v.size());
                out.flush();
                if (!out)
                {
                    if (err) *err = "写入文件失败（磁盘满？）: " + path;
                    return false;
                }
                return true;
            }

            inline u64 element_count(const std::vector<u64> &shape)
            {
                u64 n = 1;
                for (u64 d : shape) n *= d;
                return n;
            }

            /// 一个最小 JSON 值树。
            ///
            /// 只做到"能读回我们自己写的东西 + 标准文件的最小集合"：对象/数组/
            /// 字符串/数字/true/false/null。数字保留**原始文本**，因为
            /// `data_offsets` 是 u64，走 double 会在 2^53 以上丢位 —— 那种
            /// 错误只在大文件上出现，而大文件正是导出最需要的地方。
            struct JVal
            {
                enum class K
                {
                    Null,
                    Bool,
                    Num,
                    Str,
                    Arr,
                    Obj,
                } k = K::Null;
                bool b = false;
                std::string text;              // Num / Str
                std::vector<JVal> arr;         // Arr
                std::vector<std::string> keys; // Obj（与 vals 一一对应）
                std::vector<JVal> vals;

                const JVal *find(const std::string &key) const
                {
                    for (size_t i = 0; i < keys.size(); ++i)
                        if (keys[i] == key) return &vals[i];
                    return nullptr;
                }
                size_t size() const { return keys.size(); }
                bool as_u64(u64 &out) const
                {
                    if (k != K::Num) return false;
                    const char *s = text.c_str();
                    char *end = nullptr;
                    const unsigned long long v = std::strtoull(s, &end, 10);
                    if (end == s || *end != '\0') return false;
                    out = (u64)v;
                    return true;
                }
            };

            struct JParser
            {
                const char *p;
                const char *e;
                std::string err;

                void ws()
                {
                    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
                }
                bool fail(const char *why)
                {
                    if (err.empty()) err = std::string("JSON: ") + why;
                    return false;
                }
                bool str(std::string &out)
                {
                    if (p >= e || *p != '"') return fail("期望字符串");
                    ++p;
                    out.clear();
                    while (p < e && *p != '"')
                    {
                        if (*p == '\\')
                        {
                            ++p;
                            if (p >= e) return fail("转义在文件尾被截断");
                            switch (*p)
                            {
                            case '"': out.push_back('"'); break;
                            case '\\': out.push_back('\\'); break;
                            case '/': out.push_back('/'); break;
                            case 'b': out.push_back('\b'); break;
                            case 'f': out.push_back('\f'); break;
                            case 'n': out.push_back('\n'); break;
                            case 'r': out.push_back('\r'); break;
                            case 't': out.push_back('\t'); break;
                            case 'u':
                            {
                                // \uXXXX：只接受 BMP 且转成 UTF-8。代理对（emoji）
                                // 不在元数据的现实使用里，遇到就报错，而不是
                                // 悄悄写一个半个字符。
                                if (e - p < 5) return fail("\\u 被截断");
                                u32 cp = 0;
                                for (int i = 1; i <= 4; ++i)
                                {
                                    const char c = p[i];
                                    u32 d = 0;
                                    if (c >= '0' && c <= '9') d = (u32)(c - '0');
                                    else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
                                    else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
                                    else return fail("\\u 后面不是十六进制");
                                    cp = cp * 16 + d;
                                }
                                p += 4;
                                if (cp >= 0xD800 && cp <= 0xDFFF) return fail("代理对不被支持");
                                if (cp < 0x80) out.push_back((char)cp);
                                else if (cp < 0x800)
                                {
                                    out.push_back((char)(0xC0 | (cp >> 6)));
                                    out.push_back((char)(0x80 | (cp & 0x3F)));
                                }
                                else
                                {
                                    out.push_back((char)(0xE0 | (cp >> 12)));
                                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                                    out.push_back((char)(0x80 | (cp & 0x3F)));
                                }
                                break;
                            }
                            default: return fail("未知转义");
                            }
                            ++p;
                        }
                        else
                        {
                            out.push_back(*p++);
                        }
                    }
                    if (p >= e) return fail("字符串没有收尾引号");
                    ++p;
                    return true;
                }
                bool value(JVal &v, int depth = 0)
                {
                    if (depth > 32) return fail("嵌套过深");
                    ws();
                    if (p >= e) return fail("在值之前到达文件尾");
                    if (*p == '{')
                    {
                        v.k = JVal::K::Obj;
                        ++p;
                        ws();
                        if (p < e && *p == '}') { ++p; return true; }
                        while (true)
                        {
                            ws();
                            std::string key;
                            if (!str(key)) return false;
                            ws();
                            if (p >= e || *p != ':') return fail("对象缺少冒号");
                            ++p;
                            JVal val;
                            if (!value(val, depth + 1)) return false;
                            v.keys.push_back(std::move(key));
                            v.vals.push_back(std::move(val));
                            ws();
                            if (p < e && *p == ',') { ++p; continue; }
                            if (p < e && *p == '}') { ++p; return true; }
                            return fail("对象没有正确收尾");
                        }
                    }
                    if (*p == '[')
                    {
                        v.k = JVal::K::Arr;
                        ++p;
                        ws();
                        if (p < e && *p == ']') { ++p; return true; }
                        while (true)
                        {
                            JVal val;
                            if (!value(val, depth + 1)) return false;
                            v.arr.push_back(std::move(val));
                            ws();
                            if (p < e && *p == ',') { ++p; continue; }
                            if (p < e && *p == ']') { ++p; return true; }
                            return fail("数组没有正确收尾");
                        }
                    }
                    if (*p == '"')
                    {
                        v.k = JVal::K::Str;
                        return str(v.text);
                    }
                    if (e - p >= 4 && std::strncmp(p, "true", 4) == 0)
                    {
                        v.k = JVal::K::Bool; v.b = true; p += 4; return true;
                    }
                    if (e - p >= 5 && std::strncmp(p, "false", 5) == 0)
                    {
                        v.k = JVal::K::Bool; v.b = false; p += 5; return true;
                    }
                    if (e - p >= 4 && std::strncmp(p, "null", 4) == 0)
                    {
                        v.k = JVal::K::Null; p += 4; return true;
                    }
                    // 数字：整段收下，交给 as_u64 判定（含符号/小数一律拒绝）
                    const char *s = p;
                    if (p < e && (*p == '-' || *p == '+')) ++p;
                    while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                                     *p == '-' || *p == '+'))
                        ++p;
                    if (p == s) return fail("不是合法的值");
                    v.k = JVal::K::Num;
                    v.text.assign(s, (size_t)(p - s));
                    return true;
                }
            };

            inline bool json_escape(std::string &out, const std::string &s)
            {
                out.push_back('"');
                for (unsigned char ch : s)
                {
                    switch (ch)
                    {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (ch < 0x20)
                        {
                            static const char *hex = "0123456789abcdef";
                            out += "\\u00";
                            out.push_back(hex[(ch >> 4) & 0xF]);
                            out.push_back(hex[ch & 0xF]);
                        }
                        else
                        {
                            // ≥ 0x80 的字节按 UTF-8 原样通过（调用方保证是合法
                            // UTF-8；转码不做，因为"猜编码"正是要避免的事）
                            out.push_back((char)ch);
                        }
                    }
                }
                out.push_back('"');
                return true;
            }

            inline bool check_tensor(const TensorData &t, std::string *err)
            {
                if (t.name.empty())
                {
                    if (err) *err = "张量名不能为空（容器用名字索引张量）";
                    return false;
                }
                if (t.shape.empty())
                {
                    if (err) *err = "维度数为 0 的张量无法表达（标量也要写成 shape=[1]）";
                    return false;
                }
                for (u64 d : t.shape)
                    if (d == 0)
                    {
                        if (err) *err = "shape 里有 0 维度";
                        return false;
                    }
                const u64 want = element_count(t.shape) * dtype_size(t.dtype);
                if (want != (u64)t.bytes.size())
                {
                    if (err)
                        *err = "字节数与 shape×dtype 不符: " + std::to_string(t.bytes.size()) +
                               " != " + std::to_string(want);
                    return false;
                }
                return true;
            }

            inline constexpr size_t SAFETENSORS_ALIGN = 8;
        } // namespace detail

        // ============================================================
        // safetensors
        // ============================================================

        /// 写单张量 safetensors。
        ///
        /// 布局：[u64 小端] N（JSON 字节数）| JSON（空格补齐到 8 字节）| 数据
        /// JSON 里 `data_offsets` 相对**数据段起点**（不是文件起点）。
        inline bool save_safetensors(const std::string &path, const TensorData &t,
                                     const Metadata &meta, std::string *err)
        {
            using namespace detail;
            if (!check_tensor(t, err)) return false;

            std::string js = "{";
            json_escape(js, t.name);
            js += ":{\"dtype\":\"";
            js += dtype_name(t.dtype);
            js += "\",\"shape\":[";
            for (size_t i = 0; i < t.shape.size(); ++i)
            {
                if (i) js += ",";
                js += std::to_string(t.shape[i]);
            }
            js += "],\"data_offsets\":[0,";
            js += std::to_string((u64)t.bytes.size());
            js += "]}";
            if (!meta.empty())
            {
                // `__metadata__` 的值**必须是字符串**（规范如此）。非字符串没有
                // 别的去处：这是 safetensors 的表达力边界，不是可以绕开的。
                js += ",\"__metadata__\":{";
                bool first = true;
                for (const auto &kv : meta)
                {
                    if (!first) js += ",";
                    first = false;
                    json_escape(js, kv.first);
                    js += ":";
                    json_escape(js, kv.second);
                }
                js += "}";
            }
            js += "}";

            // 补齐：8（长度） + N 必须是 8 的倍数
            while ((sizeof(u64) + js.size()) % SAFETENSORS_ALIGN != 0) js.push_back(' ');

            std::vector<u8> out;
            out.reserve(8 + js.size() + t.bytes.size());
            put_u64(out, (u64)js.size());
            put_str(out, js);
            put_bytes(out, t.bytes.data(), t.bytes.size());
            return write_file(path, out, err);
        }

        /// 读 safetensors（可多张量 —— 别人的文件常常是多张量的）。
        inline bool load_safetensors(const std::string &path, std::vector<TensorData> &tensors,
                                     Metadata &meta, std::string *err)
        {
            using namespace detail;
            const std::vector<u8> buf = read_file(path, err);
            if (buf.empty() && err && !err->empty()) return false;
            if (buf.size() < 8)
            {
                if (err) *err = "safetensors: 文件太短（连头长度都没有）";
                return false;
            }

            u64 hlen = 0;
            for (int i = 0; i < 8; ++i) hlen |= (u64)buf[i] << (8 * i);
            if (hlen == 0 || hlen > (u64)buf.size() - 8)
            {
                if (err) *err = "safetensors: 头长度越界（" + std::to_string(hlen) + "）";
                return false;
            }
            const u8 *data_start = buf.data() + 8 + hlen;
            const u64 data_len = (u64)buf.size() - 8 - hlen;

            JParser jp{(const char *)buf.data() + 8, (const char *)buf.data() + 8 + hlen, {}};
            JVal root;
            if (!jp.value(root) || root.k != JVal::K::Obj)
            {
                if (err) *err = std::string("safetensors: ") + (jp.err.empty() ? "根不是对象" : jp.err);
                return false;
            }

            tensors.clear();
            meta.clear();
            for (size_t ti = 0; ti < root.size(); ++ti)
            {
                const std::string &tkey = root.keys[ti];
                const JVal &tval = root.vals[ti];
                if (tkey == "__metadata__")
                {
                    for (size_t mi = 0; mi < tval.size(); ++mi)
                    {
                        if (tval.vals[mi].k != JVal::K::Str)
                        {
                            if (err) *err = "safetensors: __metadata__ 的 " + tval.keys[mi] + " 不是字符串";
                            return false;
                        }
                        meta.emplace_back(tval.keys[mi], tval.vals[mi].text);
                    }
                    continue;
                }

                TensorData t;
                t.name = tkey;
                const JVal *dt = tval.find("dtype");
                const JVal *sh = tval.find("shape");
                const JVal *off = tval.find("data_offsets");
                if (!dt || !sh || !off || dt->k != JVal::K::Str || sh->k != JVal::K::Arr ||
                    off->k != JVal::K::Arr || off->arr.size() != 2)
                {
                    if (err) *err = "safetensors: 张量 " + tkey + " 缺少 dtype/shape/data_offsets";
                    return false;
                }
                if (!dtype_from_name(dt->text, t.dtype))
                {
                    // 不认识的 dtype（F8_E4M3 / BF16 / …）在这里就停下。映射成
                    // 别的类型会让"读进来了"变成一句谎话。
                    if (err) *err = "safetensors: 不支持的 dtype '" + dt->text + "'（只映射 F32/F64/I32/I64/U8/BOOL）";
                    return false;
                }
                const u64 n = sh->arr.size();
                t.shape.resize(n);
                for (u64 i = 0; i < n; ++i)
                    if (!sh->arr[i].as_u64(t.shape[i]))
                    {
                        if (err) *err = "safetensors: " + tkey + " 的 shape 不是正整数";
                        return false;
                    }
                u64 b = 0, e = 0;
                if (!off->arr[0].as_u64(b) || !off->arr[1].as_u64(e))
                {
                    if (err) *err = "safetensors: " + tkey + " 的 data_offsets 不是整数";
                    return false;
                }
                if (b > e || e > data_len)
                {
                    if (err) *err = "safetensors: " + tkey + " 的 data_offsets 越界";
                    return false;
                }
                const u64 want = element_count(t.shape) * dtype_size(t.dtype);
                if (e - b != want)
                {
                    if (err) *err = "safetensors: " + tkey + " 的切片长度与 shape×dtype 不符（" +
                                    std::to_string(e - b) + " != " + std::to_string(want) + "）";
                    return false;
                }
                t.bytes.assign(data_start + b, data_start + e);
                tensors.push_back(std::move(t));
            }
            return true;
        }

        // ============================================================
        // GGUF v3
        // ============================================================

        namespace gguf
        {
            constexpr u32 KV_UINT8 = 0, KV_INT8 = 1, KV_UINT16 = 2, KV_INT16 = 3, KV_UINT32 = 4,
                              KV_INT32 = 5, KV_FLOAT32 = 6, KV_BOOL = 7, KV_STRING = 8, KV_ARRAY = 9,
                              KV_UINT64 = 10, KV_INT64 = 11, KV_FLOAT64 = 12;
            constexpr u32 DEFAULT_ALIGNMENT = 32;
        } // namespace gguf

        /// 写单张量 GGUF v3。
        ///
        /// 三个必须写对的点：
        ///   - **维度逆序**：`ne[0]` 是最内层维度，而行主序 shape 的最外层在前。
        ///   - **`general.alignment` 必须先写**：读取方要靠它算数据段偏移；
        ///     值必须是非零的 2 的幂（规范要求）。
        ///   - **偏移是相对数据段起点**的，不是文件起点。
        inline bool save_gguf(const std::string &path, const TensorData &t, const Metadata &meta,
                              std::string *err)
        {
            using namespace detail;
            if (!check_tensor(t, err)) return false;

            u32 gtype = 0;
            if (!gguf_type_of(t.dtype, gtype))
            {
                if (err)
                    *err = std::string("GGUF 没有 ") + dtype_name(t.dtype) +
                           " 类型（ggml 表里只有 F32/F64/I32/I64 等）——不猜、不升位，"
                           "请换 safetensors 容器或先自行转换";
                return false;
            }

            // ── KV 段：先 general.* 再其余（有序）──
            std::string arch = "rvdlm";
            std::string name = "rvdlm-model";
            for (const auto &kv : meta)
            {
                if (kv.first == "architecture") arch = kv.second;
                else if (kv.first == "name") name = kv.second;
            }
            std::vector<std::pair<std::string, std::string>> rest;
            for (const auto &kv : meta)
                if (kv.first != "architecture" && kv.first != "name") rest.push_back(kv);

            std::vector<u8> body;   // 头之后的全部内容（KV + 张量信息 + 数据）
            auto kv_str = [&](const std::string &k, const std::string &v)
            {
                put_u64(body, (u64)k.size());
                put_str(body, k);
                put_u32(body, gguf::KV_STRING);
                put_u64(body, (u64)v.size());
                put_str(body, v);
                return 1;
            };

            u64 kv_count = 0;
            kv_count += [&] {
                put_u64(body, 17);            // strlen("general.alignment")
                put_str(body, "general.alignment");
                put_u32(body, gguf::KV_UINT32);
                put_u32(body, gguf::DEFAULT_ALIGNMENT);
                return 1;
            }();
            kv_count += kv_str("general.architecture", arch);
            kv_count += kv_str("general.name", name);
            for (const auto &kv : rest) kv_count += kv_str(kv.first, kv.second);

            // ── 张量信息 ──
            put_u64(body, (u64)t.name.size());
            put_str(body, t.name);
            put_u32(body, (u32)t.shape.size());
            for (size_t i = t.shape.size(); i-- > 0;) put_u64(body, t.shape[i]); // ← 逆序
            put_u32(body, gtype);
            put_u64(body, 0); // 唯一张量：偏移恒为 0

            // ── 头 + 对齐 + 数据 ──
            std::vector<u8> out;
            put_str(out, "GGUF");
            put_u32(out, 3);
            put_u64(out, 1); // tensor_count
            put_u64(out, kv_count);
            const size_t body_at = out.size();
            out.insert(out.end(), body.begin(), body.end());

            // 数据段起点按 alignment 对齐（对齐的是**文件**偏移，偏移字段才相对数据段）
            const size_t align = gguf::DEFAULT_ALIGNMENT;
            const size_t data_at = (out.size() + align - 1) / align * align;
            out.resize(data_at, 0);
            put_bytes(out, t.bytes.data(), t.bytes.size());
            return write_file(path, out, err);
        }

        /// 读 GGUF v3（多张量；量化类型报错）。
        inline bool load_gguf(const std::string &path, std::vector<TensorData> &tensors,
                              Metadata &meta, std::string *err)
        {
            using namespace detail;
            const std::vector<u8> buf = read_file(path, err);
            if (buf.empty() && err && !err->empty()) return false;
            Reader r{buf.data(), buf.size(), 0, {}};

            char magic[4];
            if (!r.bytes(magic, 4) || std::memcmp(magic, "GGUF", 4) != 0)
            {
                if (err) *err = "GGUF: 魔数不对";
                return false;
            }
            u32 version = 0, alignment = gguf::DEFAULT_ALIGNMENT;
            u64 n_tensors = 0, n_kv = 0;
            if (!r.ru32(version) || !r.ru64(n_tensors) || !r.ru64(n_kv))
            {
                if (err) *err = "GGUF: " + r.err;
                return false;
            }
            if (version != 3)
            {
                // v1 的维度是 u32、v2 没有对齐字段。假装读得懂 v2 会在维度上错位。
                if (err) *err = "GGUF: 只支持 v3（文件是 v" + std::to_string(version) + "）";
                return false;
            }

            auto read_kv_value = [&](u32 type, std::string &out, bool &is_str) -> bool
            {
                is_str = false;
                switch (type)
                {
                case gguf::KV_STRING:
                {
                    if (!r.gguf_str(out)) return false;
                    is_str = true;
                    return true;
                }
                case gguf::KV_UINT32:
                case gguf::KV_INT32:
                case gguf::KV_FLOAT32:
                {
                    u32 v;
                    if (!r.ru32(v)) return false;
                    if (type == gguf::KV_FLOAT32)
                    {
                        // 是**位模式**不是整数：按 u32 打印会写出 1084227584 这种数
                        float f;
                        std::memcpy(&f, &v, 4);
                        out = std::to_string(f);
                    }
                    else out = std::to_string(v);
                    return true;
                }
                case gguf::KV_UINT64:
                case gguf::KV_INT64:
                case gguf::KV_FLOAT64:
                {
                    u64 v;
                    if (!r.ru64(v)) return false;
                    if (type == gguf::KV_FLOAT64)
                    {
                        double d;
                        std::memcpy(&d, &v, 8);
                        out = std::to_string(d);
                    }
                    else out = std::to_string(v);
                    return true;
                }
                case gguf::KV_UINT8:
                case gguf::KV_INT8:
                case gguf::KV_BOOL:
                {
                    u8 v;
                    if (!r.bytes(&v, 1)) return false;
                    out = std::to_string((int)v);
                    return true;
                }
                case gguf::KV_UINT16:
                case gguf::KV_INT16:
                {
                    std::uint16_t v;
                    if (!r.ru16(v)) return false;
                    out = std::to_string(v);
                    return true;
                }
                default: return false;
                }
            };

            meta.clear();
            for (u64 i = 0; i < n_kv; ++i)
            {
                std::string key;
                if (!r.gguf_str(key))
                {
                    if (err) *err = "GGUF: " + r.err;
                    return false;
                }
                u32 type = 0;
                if (!r.ru32(type))
                {
                    if (err) *err = "GGUF: " + r.err;
                    return false;
                }
                if (type == gguf::KV_ARRAY)
                {
                    // 数组：跳过而不是存字符串（存成字符串就等于丢类型）。
                    // 元素类型、长度、然后按类型逐个跳过。
                    u32 et = 0;
                    u64 len = 0;
                    if (!r.ru32(et) || !r.ru64(len))
                    {
                        if (err) *err = "GGUF: " + r.err;
                        return false;
                    }
                    if (et == gguf::KV_STRING)
                    {
                        for (u64 k = 0; k < len; ++k)
                        {
                            std::string tmp;
                            if (!r.gguf_str(tmp)) { if (err) *err = "GGUF: " + r.err; return false; }
                        }
                    }
                    else
                    {
                        const u64 esz = (et == gguf::KV_UINT8 || et == gguf::KV_INT8 || et == gguf::KV_BOOL)
                                            ? 1
                                        : (et == gguf::KV_UINT16 || et == gguf::KV_INT16) ? 2
                                        : (et == gguf::KV_UINT64 || et == gguf::KV_INT64 ||
                                           et == gguf::KV_FLOAT64) ? 8
                                                                   : 4;
                        if (len > (u64)buf.size() || !r.need((size_t)(len * esz)))
                        {
                            if (err) *err = "GGUF: 数组长度越界";
                            return false;
                        }
                        r.at += (size_t)(len * esz);
                    }
                    continue;
                }
                std::string val;
                bool is_str = false;
                if (!read_kv_value(type, val, is_str))
                {
                    if (err) *err = "GGUF: KV '" + key + "' 的值类型 " + std::to_string(type) + " 读不了";
                    return false;
                }
                meta.emplace_back(key, val);
                // ⚠ 这里**不能**要求 is_str：general.alignment 是 UINT32，
                //   按"只有字符串才解析"写，对齐值永远不生效，读到多张量文件
                //   才开始错位。类型分支已经把它转成十进制文本了。
                (void)is_str;
                if (key == "general.alignment")
                {
                    u64 a = 0;
                    try { a = std::stoull(val); } catch (...) { a = 0; }
                    if (a == 0 || (a & (a - 1)) != 0)
                    {
                        if (err) *err = "GGUF: general.alignment 不是 2 的幂";
                        return false;
                    }
                    alignment = (u32)a;
                }
            }

            struct Info
            {
                std::string name;
                std::vector<u64> shape;
                u32 gtype = 0;
                u64 off = 0;
            };
            std::vector<Info> infos((size_t)n_tensors);
            for (u64 i = 0; i < n_tensors; ++i)
            {
                Info &in = infos[(size_t)i];
                u32 nd = 0;
                if (!r.gguf_str(in.name) || !r.ru32(nd))
                {
                    if (err) *err = "GGUF: " + r.err;
                    return false;
                }
                // 逆序读回：ne[0] 是最内层 ⇒ 反转成行主序
                std::vector<u64> dims(nd);
                for (u32 k = 0; k < nd; ++k)
                    if (!r.ru64(dims[k])) { if (err) *err = "GGUF: " + r.err; return false; }
                in.shape.assign(dims.rbegin(), dims.rend());
                if (!r.ru32(in.gtype) || !r.ru64(in.off))
                {
                    if (err) *err = "GGUF: " + r.err;
                    return false;
                }
            }

            const size_t data_at = (r.at + alignment - 1) / alignment * alignment;
            if (data_at > buf.size())
            {
                if (err) *err = "GGUF: 数据段起点越界";
                return false;
            }

            tensors.clear();
            for (const Info &in : infos)
            {
                TensorData t;
                t.name = in.name;
                t.shape = in.shape;
                if (!dtype_of_gguf(in.gtype, t.dtype))
                {
                    if (err) *err = "GGUF: 张量 " + in.name + " 的类型 " + std::to_string(in.gtype) +
                                    " 不是本文件映射的四种（F32/F64/I32/I64）——量化类型不假装支持";
                    return false;
                }
                const u64 want = element_count(t.shape) * dtype_size(t.dtype);
                const size_t at = data_at + (size_t)in.off;
                if (at + want > buf.size())
                {
                    if (err) *err = "GGUF: 张量 " + in.name + " 的数据越界";
                    return false;
                }
                t.bytes.assign(buf.begin() + (std::ptrdiff_t)at, buf.begin() + (std::ptrdiff_t)(at + want));
                tensors.push_back(std::move(t));
            }
            return true;
        }

        // ============================================================
        // `.rvdlm` 入口（类型由模板参数声明，见文件头第 1 条）
        // ============================================================

        /// 只做形状/字节数校验（与 detail::check_tensor 同一判据，暴露给调用方复用）。
        inline bool check_tensor_public(const TensorData &t, std::string *err)
        {
            return detail::check_tensor(t, err);
        }

        template <typename T>
        inline bool rvdlm_to_tensor(const std::string &in_path, const std::string &tensor_name,
                                    TensorData &out, Metadata &meta, std::string *err)
        {
            RVDLModel<T> model = ModelExporter<T>::load(in_path); // 失败抛异常 —— 由调用方接
            out.name = tensor_name.empty() ? "weight" : tensor_name;
            out.dtype = dtype_of<T>::value;
            out.shape.assign(model.tensor.get_shape().begin(), model.tensor.get_shape().end());
            const size_t n = model.tensor.size() * sizeof(T);
            out.bytes.resize(n);
            std::memcpy(out.bytes.data(), model.tensor.data(), n);
            meta = sorted_metadata(model.metadata);
            return check_tensor_public(out, err);
        }

        template <typename T>
        inline bool export_rvdlm_to_safetensors(const std::string &in_path, const std::string &out_path,
                                                const std::string &tensor_name = "weight",
                                                std::string *err = nullptr)
        {
            TensorData t;
            Metadata meta;
            if (!rvdlm_to_tensor<T>(in_path, tensor_name, t, meta, err)) return false;
            return save_safetensors(out_path, t, meta, err);
        }

        template <typename T>
        inline bool export_rvdlm_to_gguf(const std::string &in_path, const std::string &out_path,
                                         const std::string &tensor_name = "weight",
                                         std::string *err = nullptr)
        {
            TensorData t;
            Metadata meta;
            if (!rvdlm_to_tensor<T>(in_path, tensor_name, t, meta, err)) return false;
            return save_gguf(out_path, t, meta, err);
        }

        /// `.rvdlm` 元素类型名（命令行的 f32/f64/i32/i64/u8 后缀）→ 分发。
        /// 返回 false 表示未知类型名；`ok` 是导出结果。
        template <typename Fn>
        inline bool with_rvdlm_type(const std::string &type_name, Fn &&fn, bool &ok)
        {
            if (type_name == "f32") { ok = fn.template operator()<float>(); return true; }
            if (type_name == "f64") { ok = fn.template operator()<double>(); return true; }
            if (type_name == "i32") { ok = fn.template operator()<std::int32_t>(); return true; }
            if (type_name == "i64") { ok = fn.template operator()<std::int64_t>(); return true; }
            if (type_name == "u8") { ok = fn.template operator()<u8>(); return true; }
            return false;
        }

        // ============================================================
        // 反向导入：safetensors / gguf → `.rvdlm`
        //
        // 与导出方向的根本差异：容器**自带 dtype**（safetensors 的 JSON 里有
        // `dtype`，GGUF 的张量信息段里有 ggml 类型号），而 `.rvdlm` 的
        // QKMHeader **不记录**。因此导入侧的类型权威来源是容器本身 ——
        // 用 with_container_type() 按容器 dtype 分发，而不是要求用户声明。
        // 手动指定 T 的入口仍然保留（与导出的模板形态对称），但会先用
        // dtype_of<T>::value 与容器 dtype 对账，不一致即报错 ——
        // 「按错的类型读不会报错，只会得到垃圾」这条纪律在两侧都成立。
        // ============================================================

        /// 从读回的容器张量中选出目标张量。
        /// tensor_name 为空 → 取第一个（单张量容器的常见形态）；
        /// 否则按名匹配 —— 多张量容器必须指明，**不猜**（没找到时列出可用名，
        /// 逐个试名字是盲猜）。
        inline bool pick_tensor(const std::vector<TensorData> &tensors,
                                const std::string &tensor_name,
                                TensorData &out, std::string *err)
        {
            if (tensors.empty())
            {
                if (err) *err = "容器内没有张量";
                return false;
            }
            if (tensor_name.empty())
            {
                out = tensors.front();
                return true;
            }
            for (const auto &t : tensors)
                if (t.name == tensor_name) { out = t; return true; }
            std::string names;
            for (size_t i = 0; i < tensors.size(); ++i)
                names += (i ? ", " : "") + tensors[i].name;
            if (err) *err = "容器内没有名为 '" + tensor_name + "' 的张量；可用：" + names;
            return false;
        }

        /// GGUF 的 KV 段 → `.rvdlm` 元数据的反向映射。
        ///
        /// `save_gguf` 把 `.rvdlm` 元数据的 `name` / `architecture` 两个键
        /// **吸收**进 `general.name` / `general.architecture`（并为缺失者补默认值，
        /// 以兼容 llama.cpp 生态对这两个键的预期），同时写 `general.alignment`
        /// 格式参数。导入时反向映射：`general.name` → `name`、
        /// `general.architecture` → `architecture`、`general.alignment` 与其余
        /// `general.*`（别的工具的文件级元数据）一律丢弃。
        ///
        /// 由此得到的 round-trip 契约是**不丢失的单调超集**：原元数据的每个
        /// 键值对都完好回来，唯一可能多出的是 `architecture`（源元数据没写它时，
        /// 导出侧补了默认值 "rvdlm"，反向映射无从区分「补的」与「写的」）。
        /// 没有这层映射，`rvdlm → gguf → rvdlm` 会**静默丢掉** name/architecture
        /// 并多出三个格式键 —— 那不是守恒。
        inline Metadata gguf_meta_to_rvdlm(const Metadata &meta)
        {
            Metadata out;
            for (const auto &kv : meta)
            {
                if (kv.first == "general.alignment") continue;
                if (kv.first == "general.name") { out.emplace_back("name", kv.second); continue; }
                if (kv.first == "general.architecture") { out.emplace_back("architecture", kv.second); continue; }
                if (kv.first.rfind("general.", 0) == 0) continue;
                out.push_back(kv);
            }
            std::sort(out.begin(), out.end(),
                      [](const std::pair<std::string, std::string> &a,
                         const std::pair<std::string, std::string> &b) { return a.first < b.first; });
            return out;
        }

        /// TensorData + 元数据 → `.rvdlm`（QKM1 单张量格式）。
        /// 类型对账失败即报错（见本节头部说明）。
        template <typename T>
        inline bool tensor_to_rvdlm(const TensorData &t, const Metadata &meta,
                                    const std::string &out_path, std::string *err)
        {
            if (t.dtype != dtype_of<T>::value)
            {
                if (err) *err = std::string("类型不一致：容器记录 ") + dtype_name(t.dtype)
                                + "，但目标 .rvdlm 按 " + dtype_name(dtype_of<T>::value)
                                + " 写入 —— 按错的类型重组不会报错，只会得到垃圾";
                return false;
            }
            std::vector<size_t> shape(t.shape.begin(), t.shape.end());
            const size_t want = detail::element_count(shape) * sizeof(T);
            if (t.bytes.size() != want)
            {
                if (err) *err = "张量 '" + t.name + "' 字节数与 shape×dtype 不符（"
                                + std::to_string(t.bytes.size()) + " ≠ " + std::to_string(want) + "）";
                return false;
            }
            numqk::Tensor<T> tensor(shape, false);
            if (!t.bytes.empty())
                std::memcpy(tensor.data(), t.bytes.data(), t.bytes.size());
            std::unordered_map<std::string, std::string> md;
            for (const auto &kv : meta) md.emplace(kv.first, kv.second);
            RVDLModel<T> model(tensor, md);
            try
            {
                ModelExporter<T>::save(out_path, model);
            }
            catch (const std::exception &e)
            {
                if (err) *err = std::string(".rvdlm 写出失败：") + e.what();
                return false;
            }
            return true;
        }

        template <typename T>
        inline bool import_safetensors_to_rvdlm(const std::string &in_path,
                                                const std::string &out_path,
                                                const std::string &tensor_name = "",
                                                std::string *err = nullptr)
        {
            std::vector<TensorData> tensors;
            Metadata meta;
            if (!load_safetensors(in_path, tensors, meta, err)) return false;
            TensorData t;
            if (!pick_tensor(tensors, tensor_name, t, err)) return false;
            return tensor_to_rvdlm<T>(t, meta, out_path, err);
        }

        template <typename T>
        inline bool import_gguf_to_rvdlm(const std::string &in_path,
                                         const std::string &out_path,
                                         const std::string &tensor_name = "",
                                         std::string *err = nullptr)
        {
            std::vector<TensorData> tensors;
            Metadata meta;
            if (!load_gguf(in_path, tensors, meta, err)) return false;
            TensorData t;
            if (!pick_tensor(tensors, tensor_name, t, err)) return false;
            return tensor_to_rvdlm<T>(t, gguf_meta_to_rvdlm(meta), out_path, err);
        }

        /// 容器 dtype → C++ 类型分发（导入侧的权威类型来源）。
        /// BOOL **故意排除**：`.rvdlm` 的载体是 `numqk::Tensor<T>`，其
        /// `data()` 转发 `std::vector<T>::data()` —— 而 `std::vector<bool>`
        /// 是标准库特化，**没有** `data()`（位压缩存储）。把 bool 张量写进
        /// `.rvdlm` 要么编译不过，要么得按位拆包后谎称另一个 dtype ——
        /// 两者都是「假装支持」。故 BOOL 容器导入直接报错，由调用方
        /// 先转成 U8/I32 再导入。返回 false 即表示该 dtype 无对应 C++ 类型。
        template <typename Fn>
        inline bool with_container_type(DType dt, Fn &&fn, bool &ok)
        {
            switch (dt)
            {
            case DType::F32:  ok = fn.template operator()<float>();               return true;
            case DType::F64:  ok = fn.template operator()<double>();              return true;
            case DType::I32:  ok = fn.template operator()<std::int32_t>();        return true;
            case DType::I64:  ok = fn.template operator()<std::int64_t>();        return true;
            case DType::U8:   ok = fn.template operator()<u8>();                  return true;
            case DType::BOOL: return false;   // 见上方说明：无对应 C++ 类型
            }
            return false;
        }

        /// 按容器 dtype 自动分发（无需调用方声明类型）。
        /// 先读容器拿到 dtype，再分派到对应 T —— 这与导出侧「必须声明类型」
        /// 的不对称是**信息不对称**：容器记录 dtype，`.rvdlm` 不记录。
        inline bool import_safetensors_to_rvdlm_auto(const std::string &in_path,
                                                     const std::string &out_path,
                                                     const std::string &tensor_name = "",
                                                     std::string *err = nullptr)
        {
            std::vector<TensorData> tensors;
            Metadata meta;
            if (!load_safetensors(in_path, tensors, meta, err)) return false;
            TensorData t;
            if (!pick_tensor(tensors, tensor_name, t, err)) return false;
            bool ok = false;
            const bool known = with_container_type(t.dtype,
                [&]<typename T>() { return tensor_to_rvdlm<T>(t, meta, out_path, err); }, ok);
            if (!known)
            {
                if (err) *err = std::string("容器 dtype ") + dtype_name(t.dtype)
                                + " 无对应 C++ 类型（BOOL 受限于 std::vector<bool> 无 data()，"
                                   "`.rvdlm` 载体 numqk::Tensor<bool> 不可用 —— 先转成 U8/I32 再导入）";
                return false;
            }
            return ok;
        }

        inline bool import_gguf_to_rvdlm_auto(const std::string &in_path,
                                              const std::string &out_path,
                                              const std::string &tensor_name = "",
                                              std::string *err = nullptr)
        {
            std::vector<TensorData> tensors;
            Metadata meta;
            if (!load_gguf(in_path, tensors, meta, err)) return false;
            TensorData t;
            if (!pick_tensor(tensors, tensor_name, t, err)) return false;
            const Metadata md = gguf_meta_to_rvdlm(meta);
            bool ok = false;
            const bool known = with_container_type(t.dtype,
                [&]<typename T>() { return tensor_to_rvdlm<T>(t, md, out_path, err); }, ok);
            if (!known)
            {
                if (err) *err = std::string("容器 dtype ") + dtype_name(t.dtype)
                                + " 无对应 C++ 类型（BOOL 受限于 std::vector<bool> 无 data()，"
                                   "`.rvdlm` 载体 numqk::Tensor<bool> 不可用 —— 先转成 U8/I32 再导入）";
                return false;
            }
            return ok;
        }

    } // namespace interop
} // namespace qml
