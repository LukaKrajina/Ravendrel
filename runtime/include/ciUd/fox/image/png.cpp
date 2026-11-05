// ============================================================
// fox/image/png.cpp —— 自写 inflate + PNG 解码 + mipmap 链 + 三线性采样
//
// 见 image/png.hpp 开头：为什么自写、边界在哪、哪些不做。
//
// 这一层的正确性证据不是"看着对"，而是：
//   · RFC1951 的三种块（stored / fixed / dynamic）都被测试向量走到 ——
//     InflateStats 会证明这一点，而不是靠"我觉得覆盖了"；
//   · zlib 自己产出的流解得回原字节（adler32 是第二道锁）；
//   · PNG 的每个行滤波器类型都有一张手写的图；
//   · 负例（坏 CRC、截断、非法 btype、回溯越界）必须**失败**，不能解出垃圾。
// ============================================================

#include "image/png.hpp"
#include <cmath>
#include <cstring>

namespace fox
{
    namespace img
    {
        namespace
        {
            // ── CRC-32（PNG 每块校验）───────────────────────────────────
            u32 crc32_bytes(const u8 *p, size_t n)
            {
                static u32 table[256];
                static bool ready = false;
                if (!ready)
                {
                    for (u32 i = 0; i < 256; ++i)
                    {
                        u32 c = i;
                        for (int k = 0; k < 8; ++k)
                            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
                        table[i] = c;
                    }
                    ready = true;
                }
                u32 c = 0xFFFFFFFFu;
                for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
                return c ^ 0xFFFFFFFFu;
            }

            // ── Adler-32（zlib 尾部校验）────────────────────────────────
            u32 adler32_bytes(const u8 *p, size_t n)
            {
                u32 s1 = 1, s2 = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    s1 += p[i];
                    if (s1 >= 65521u) s1 -= 65521u;
                    s2 += s1;
                    if (s2 >= 65521u) s2 -= 65521u;
                }
                return (s2 << 16) | s1;
            }

            u32 be32(const u8 *p)
            {
                return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
            }

            void set_err(std::string *err, const char *msg)
            {
                if (err) *err = msg;
            }

            // ── 位读取器 ────────────────────────────────────────────────
            //
            // DEFLATE 把位按 LSB-first 塞进字节；而 Huffman **码**是按
            // MSB-first 排的 —— 于是"读一位"在这里返回的正是码的下一位，
            // 而 get(n)（LEN、extra bits）返回的是普通的低位在前整数。
            // 两种顺序共用一个读取器，是 RFC1951 最容易写错的地方。
            struct Bits
            {
                const u8 *p = nullptr;
                size_t n = 0;
                size_t i = 0;
                u32 buf = 0;
                u32 cnt = 0;
                bool over = false;

                u32 get(u32 need)
                {
                    while (cnt < need)
                    {
                        if (i >= n)
                        {
                            over = true;
                            return 0;
                        }
                        buf |= (u32)p[i++] << cnt;
                        cnt += 8;
                    }
                    const u32 v = buf & ((need >= 32) ? 0xFFFFFFFFu : ((1u << need) - 1u));
                    buf >>= need;
                    cnt -= need;
                    return v;
                }

                /// 丢掉当前字节里剩下的填充位，使后续读取按字节对齐。
                ///
                /// ⚠ 要丢的是 `cnt % 8` 位，不是 `(8 - cnt % 8) % 8`。
                ///   缓冲里剩下的位就是当前字节的剩余位，对齐 = 把它们全丢掉。
                ///   写成后者时约 3/4 的流仍能解对（两种公式在 cnt%8 ∈ {0,4}
                ///   时重合），错的那 1/4 报出来的是"Adler-32 不匹配"——
                ///   一个看起来像校验和问题的位对齐 bug。
                void align()
                {
                    const u32 pad = cnt % 8u;
                    buf >>= pad;
                    cnt -= pad;
                }
            };

            // ── 规范 Huffman 表 ─────────────────────────────────────────
            struct Huff
            {
                u16 count[16] = {};
                u16 symbol[288] = {};
            };

            /// 从码长表构造解码表。返回 false 时 `*err` 有话说。
            ///
            /// `allow_incomplete`：DEFLATE 允许"不完整"的码树（只有一条码的
            /// 距离树就是这么来的），不允许的情况是**过满**的树（left < 0）。
            bool build_huff(const u8 *lengths, int n, Huff &h, bool allow_incomplete,
                            std::string *err)
            {
                for (int i = 0; i < 16; ++i) h.count[i] = 0;
                for (int i = 0; i < n; ++i) h.count[lengths[i]]++;

                int left = 1;
                for (int len = 1; len <= 15; ++len)
                {
                    left <<= 1;
                    left -= h.count[len];
                    if (left < 0)
                    {
                        set_err(err, "over-subscribed Huffman tree");
                        return false;
                    }
                }
                if (left > 0 && !allow_incomplete)
                {
                    set_err(err, "incomplete Huffman tree");
                    return false;
                }

                h.count[0] = 0; // 码长 0 = 没有这条码，不参与排序
                u16 offs[16];
                offs[1] = 0;
                for (int len = 1; len < 15; ++len)
                    offs[len + 1] = (u16)(offs[len] + h.count[len]);
                for (int s = 0; s < n; ++s)
                    if (lengths[s]) h.symbol[offs[lengths[s]]++] = (u16)s;
                return true;
            }

            /// 解一个 Huffman 符号（规范码，MSB-first 组装）。
            int decode_sym(Bits &b, const Huff &h)
            {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len <= 15; ++len)
                {
                    code |= (int)b.get(1);
                    const int count = h.count[len];
                    if (code - first < count) return h.symbol[index + (code - first)];
                    index += count;
                    first = (first + count) << 1;
                    code <<= 1;
                }
                return -1;
            }

            const u16 kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            const u8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            const u16 kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                       33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                       1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
            const u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

            bool raw_copy(Bits &b, size_t len, std::vector<u8> &out, size_t max_out,
                          std::string *err)
            {
                if (out.size() + len > max_out)
                {
                    set_err(err, "inflate output exceeds limit");
                    return false;
                }
                while (len && b.cnt >= 8)
                {
                    out.push_back((u8)(b.buf & 0xFF));
                    b.buf >>= 8;
                    b.cnt -= 8;
                    --len;
                }
                while (len)
                {
                    if (b.i >= b.n)
                    {
                        set_err(err, "truncated stored block");
                        return false;
                    }
                    out.push_back(b.p[b.i++]);
                    --len;
                }
                return true;
            }

            bool inflate_block(Bits &b, std::vector<u8> &out, size_t max_out, InflateStats &stats,
                               std::string *err)
            {
                bool final = false;
                do
                {
                    const u32 bfinal = b.get(1);
                    const u32 btype = b.get(2);
                    if (b.over)
                    {
                        set_err(err, "truncated deflate stream");
                        return false;
                    }

                    if (btype == 0)
                    {
                        ++stats.stored;
                        b.align();
                        const u32 len = b.get(16);
                        const u32 nlen = b.get(16);
                        if (b.over)
                        {
                            set_err(err, "truncated stored block header");
                            return false;
                        }
                        if ((len ^ 0xFFFFu) != nlen)
                        {
                            set_err(err, "stored block length mismatch");
                            return false;
                        }
                        if (!raw_copy(b, len, out, max_out, err)) return false;
                    }
                    else if (btype == 1 || btype == 2)
                    {
                        // 表是每次现建的局部量，不是 static：inflate 可能被
                        // 多个线程同时调用（两张图并行解码），而\"static 表 +
                        // ready 标志\"恰恰是那种在单线程测试里永远不炸的写法。
                        // 建表成本是几百次自增，相对解码几千个符号可以忽略。
                        Huff fixed_lit, fixed_dist;
                        u8 lit_len[288], dist_len[32];

                        const Huff *lit = nullptr;
                        const Huff *dist = nullptr;

                        if (btype == 1)
                        {
                            ++stats.fixed;
                            for (int i = 0; i < 144; ++i) lit_len[i] = 8;
                            for (int i = 144; i < 256; ++i) lit_len[i] = 9;
                            for (int i = 256; i < 280; ++i) lit_len[i] = 7;
                            for (int i = 280; i < 288; ++i) lit_len[i] = 8;
                            for (int i = 0; i < 32; ++i) dist_len[i] = 5;
                            std::string e;
                            if (!build_huff(lit_len, 288, fixed_lit, true, &e) ||
                                !build_huff(dist_len, 32, fixed_dist, true, &e))
                                return false;
                            lit = &fixed_lit;
                            dist = &fixed_dist;
                        }
                        else
                        {
                            ++stats.dynamic;
                            const u32 hlit = b.get(5) + 257;
                            const u32 hdist = b.get(5) + 1;
                            const u32 hclen = b.get(4) + 4;
                            if (b.over || hlit > 286 + 1 || hdist > 30 + 1)
                            {
                                set_err(err, "bad dynamic block header");
                                return false;
                            }

                            static const u8 order[19] = {16, 17, 18, 0, 8,  7,  9,  6,  10, 5,
                                                         11, 4,  12, 3, 13, 2,  14, 1,  15};
                            u8 cl_len[19] = {};
                            for (u32 i = 0; i < hclen; ++i) cl_len[order[i]] = (u8)b.get(3);
                            if (b.over)
                            {
                                set_err(err, "truncated code-length table");
                                return false;
                            }
                            Huff cl;
                            if (!build_huff(cl_len, 19, cl, false, err)) return false;

                            u8 lens[288 + 32] = {};
                            const int total = (int)(hlit + hdist);
                            int i = 0;
                            while (i < total)
                            {
                                const int sym = decode_sym(b, cl);
                                if (sym < 0)
                                {
                                    set_err(err, "bad code-length symbol");
                                    return false;
                                }
                                if (sym < 16)
                                {
                                    lens[i++] = (u8)sym;
                                }
                                else if (sym == 16)
                                {
                                    if (i == 0)
                                    {
                                        set_err(err, "repeat with no previous length");
                                        return false;
                                    }
                                    const u32 rep = 3 + b.get(2);
                                    const u8 prev = lens[i - 1];
                                    for (u32 r = 0; r < rep && i < total; ++r) lens[i++] = prev;
                                }
                                else if (sym == 17)
                                {
                                    const u32 rep = 3 + b.get(3);
                                    for (u32 r = 0; r < rep && i < total; ++r) lens[i++] = 0;
                                }
                                else
                                {
                                    const u32 rep = 11 + b.get(7);
                                    for (u32 r = 0; r < rep && i < total; ++r) lens[i++] = 0;
                                }
                                if (b.over)
                                {
                                    set_err(err, "truncated code-length list");
                                    return false;
                                }
                            }

                            // 记录用的 lit/dist 表：局部量，理由同上面的 fixed 表。
                            Huff dyn_lit, dyn_dist;
                            std::string e;
                            // 字面量/长度树：允许"不完整"仅限单码（RFC 只允许
                            // 这一种退化情形）。
                            if (!build_huff(lens, (int)hlit, dyn_lit, true, &e))
                            {
                                set_err(err, e.empty() ? "bad literal tree" : e.c_str());
                                return false;
                            }
                            // 距离树：单码（甚至只用一个距离码）是常规情形。
                            if (!build_huff(lens + hlit, (int)hdist, dyn_dist, true, &e))
                            {
                                set_err(err, e.empty() ? "bad distance tree" : e.c_str());
                                return false;
                            }
                            lit = &dyn_lit;
                            dist = &dyn_dist;
                        }

                        // ── LZ77 主循环 ──
                        for (;;)
                        {
                            // 截断的流必须在这里被抓住。位读取器在耗尽输入后
                            // 会一直返回 0，而全零在固定 Huffman 表里是一条合法
                            // 的码（EOB）—— 不查 b.over，一个被砍掉一半的流会
                            // "成功"解出一段垃圾，然后在 Adler-32 上莫名其妙地
                            // 失败（或者更糟：解出半张图）。
                            if (b.over)
                            {
                                set_err(err, "truncated deflate stream");
                                return false;
                            }
                            const int sym = decode_sym(b, *lit);
                            if (sym < 0)
                            {
                                set_err(err, "bad literal/length symbol");
                                return false;
                            }
                            if (sym < 256)
                            {
                                if (out.size() >= max_out)
                                {
                                    set_err(err, "inflate output exceeds limit");
                                    return false;
                                }
                                out.push_back((u8)sym);
                            }
                            else if (sym == 256)
                            {
                                break;
                            }
                            else
                            {
                                const int li = sym - 257;
                                if (li >= 29)
                                {
                                    set_err(err, "invalid length symbol");
                                    return false;
                                }
                                const u32 len = kLenBase[li] + b.get(kLenExtra[li]);
                                const int ds = decode_sym(b, *dist);
                                if (ds < 0 || ds >= 30)
                                {
                                    set_err(err, "invalid distance symbol");
                                    return false;
                                }
                                const u32 dst = kDistBase[ds] + b.get(kDistExtra[ds]);
                                if (b.over)
                                {
                                    set_err(err, "truncated match");
                                    return false;
                                }
                                if (dst > out.size())
                                {
                                    set_err(err, "distance points before the output start");
                                    return false;
                                }
                                if (out.size() + len > max_out)
                                {
                                    set_err(err, "inflate output exceeds limit");
                                    return false;
                                }
                                // 逐字节：重叠拷贝（dst < len）是 LZ77 的常规
                                // 情形（游程编码就是它做出来的），memcpy 会错。
                                const size_t from = out.size() - dst;
                                for (u32 k = 0; k < len; ++k) out.push_back(out[from + k]);
                            }
                        }
                    }
                    else
                    {
                        set_err(err, "reserved deflate block type 3");
                        return false;
                    }

                    final = bfinal != 0;
                } while (!final);
                return true;
            }
        } // namespace

        bool inflate_zlib(const u8 *src, size_t n, std::vector<u8> &out, InflateStats *stats,
                          std::string *err, size_t max_out)
        {
            out.clear();
            if (!src || n < 2)
            {
                set_err(err, "zlib stream too short");
                return false;
            }
            // 头部的两个字节先查：一个 4 字节的垃圾流更值得说"这不是 deflate"
            // 而不是"太短"—— 长度检查放在最后，报出的原因才是真正的原因。
            const u32 cmf = src[0], flg = src[1];
            if ((cmf & 0x0F) != 8)
            {
                set_err(err, "zlib: not deflate");
                return false;
            }
            if (((cmf << 8) | flg) % 31u != 0)
            {
                set_err(err, "zlib: header check failed");
                return false;
            }
            if (flg & 0x20)
            {
                set_err(err, "zlib: preset dictionary not supported");
                return false;
            }
            if (n < 6)
            {
                set_err(err, "zlib stream too short");
                return false;
            }

            Bits b;
            b.p = src + 2;
            b.n = n - 2;

            InflateStats local;
            if (!inflate_block(b, out, max_out, local, err)) return false;
            if (stats) *stats = local;

            // 尾部 4 字节 Adler-32（大端）。align() 丢掉最后一个字节里的
            // 填充位；位缓冲区里**可能**已经预读进了 Adler 的前几个字节，
            // 所以这 4 个字节也从同一个读取器里取。
            b.align();
            const u32 a0 = b.get(8), a1 = b.get(8), a2 = b.get(8), a3 = b.get(8);
            if (b.over)
            {
                set_err(err, "zlib: missing Adler-32");
                return false;
            }
            const u32 want = (a0 << 24) | (a1 << 16) | (a2 << 8) | a3;
            const u32 have = adler32_bytes(out.data(), out.size());
            if (want != have)
            {
                set_err(err, "zlib: Adler-32 mismatch");
                return false;
            }
            return true;
        }

        // ── PNG ──────────────────────────────────────────────────────────

        namespace
        {
            int channels_of(int color_type)
            {
                switch (color_type)
                {
                case 0: return 1; // 灰度
                case 2: return 3; // RGB
                case 3: return 1; // 调色板索引
                case 4: return 2; // 灰度 + alpha
                case 6: return 4; // RGBA
                default: return 0;
                }
            }

            bool depth_ok(int color_type, int depth)
            {
                switch (color_type)
                {
                case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
                case 2: return depth == 8 || depth == 16;
                case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
                case 4: return depth == 8 || depth == 16;
                case 6: return depth == 8 || depth == 16;
                default: return false;
                }
            }

            inline u8 paeth(u8 a, u8 b2, u8 c)
            {
                const int p = (int)a + (int)b2 - (int)c;
                const int pa = p > a ? p - a : a - p;
                const int pb = p > b2 ? p - b2 : b2 - p;
                const int pc = p > c ? p - c : c - p;
                if (pa <= pb && pa <= pc) return a;
                if (pb <= pc) return b2;
                return c;
            }

            /// 取子字节样本（PNG 在字节内 MSB-first 打包）。
            inline u32 sub_sample(const u8 *row, u32 x, int depth)
            {
                const u32 bit = x * (u32)depth;
                const u32 byte = row[bit >> 3];
                const u32 shift = 8u - (u32)depth - (bit & 7u);
                return (byte >> shift) & ((1u << (u32)depth) - 1u);
            }
        } // namespace

        bool decode_png(const u8 *src, size_t n, Image &out, std::string *err)
        {
            static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
            if (!src || n < 8 || std::memcmp(src, sig, 8) != 0)
            {
                set_err(err, "not a PNG (bad signature)");
                return false;
            }

            u32 w = 0, h = 0;
            int depth = 0, ctype = -1;
            bool interlace = false, seen_ihdr = false, seen_iend = false;
            std::vector<u8> plte, trns, idat;

            size_t p = 8;
            while (p + 12 <= n)
            {
                const u32 len = be32(src + p);
                const char *type = (const char *)(src + p + 4);
                if (len > n - p - 12)
                {
                    set_err(err, "truncated chunk");
                    return false;
                }
                const u8 *data = src + p + 8;
                const u32 crc = be32(src + p + 8 + len);
                const u32 crc_have = crc32_bytes(src + p + 4, (size_t)len + 4);
                if (crc != crc_have)
                {
                    std::string m = "CRC mismatch in chunk ";
                    m.append(type, 4);
                    set_err(err, m.c_str());
                    return false;
                }

                if (std::memcmp(type, "IHDR", 4) == 0)
                {
                    if (len != 13)
                    {
                        set_err(err, "IHDR has the wrong size");
                        return false;
                    }
                    w = be32(data);
                    h = be32(data + 4);
                    depth = data[8];
                    ctype = data[9];
                    if (data[10] != 0)
                    {
                        set_err(err, "unsupported compression method");
                        return false;
                    }
                    if (data[11] != 0)
                    {
                        set_err(err, "unsupported filter method");
                        return false;
                    }
                    interlace = data[12] != 0;
                    seen_ihdr = true;
                }
                else if (std::memcmp(type, "PLTE", 4) == 0)
                {
                    plte.assign(data, data + len);
                }
                else if (std::memcmp(type, "tRNS", 4) == 0)
                {
                    trns.assign(data, data + len);
                }
                else if (std::memcmp(type, "IDAT", 4) == 0)
                {
                    idat.insert(idat.end(), data, data + len);
                }
                else if (std::memcmp(type, "IEND", 4) == 0)
                {
                    seen_iend = true;
                    break;
                }
                // 其它块（gAMA / pHYs / iCCP / tEXt / APNG 的 acTL…）被忽略：
                // 它们不改变像素语义，而"读不懂就报错"会让常见文件打不开。
                p += 12 + (size_t)len;
            }

            if (!seen_ihdr)
            {
                set_err(err, "no IHDR chunk");
                return false;
            }
            if (!seen_iend)
            {
                set_err(err, "no IEND chunk");
                return false;
            }
            if (interlace)
            {
                set_err(err, "Adam7 interlace is not supported");
                return false;
            }
            if (w == 0 || h == 0 || w > 16384 || h > 16384)
            {
                set_err(err, "bad PNG dimensions");
                return false;
            }
            const int ch = channels_of(ctype);
            if (ch == 0 || !depth_ok(ctype, depth))
            {
                set_err(err, "unsupported color type / bit depth combination");
                return false;
            }
            if (ctype == 3 && plte.size() < 3)
            {
                set_err(err, "palette image without PLTE");
                return false;
            }
            if (idat.empty())
            {
                set_err(err, "no IDAT data");
                return false;
            }

            std::vector<u8> raw;
            {
                std::string e;
                if (!inflate_zlib(idat.data(), idat.size(), raw, nullptr, &e))
                {
                    std::string m = "IDAT inflate failed: ";
                    m += e;
                    set_err(err, m.c_str());
                    return false;
                }
            }

            const u32 bits_pp = (u32)depth * (u32)ch;
            const size_t rowbytes = ((size_t)w * bits_pp + 7) / 8;
            const size_t bpp = bits_pp >= 8 ? bits_pp / 8 : 1;
            const size_t need = (size_t)h * (rowbytes + 1);
            if (raw.size() < need)
            {
                set_err(err, "IDAT shorter than the image");
                return false;
            }

            // ── 行滤波器 ──
            std::vector<u8> rows((size_t)h * rowbytes);
            for (u32 y = 0; y < h; ++y)
            {
                const u8 ft = raw[(size_t)y * (rowbytes + 1)];
                const u8 *in_row = raw.data() + (size_t)y * (rowbytes + 1) + 1;
                u8 *dst = rows.data() + (size_t)y * rowbytes;
                const u8 *up = y ? rows.data() + (size_t)(y - 1) * rowbytes : nullptr;

                switch (ft)
                {
                case 0:
                    std::memcpy(dst, in_row, rowbytes);
                    break;
                case 1:
                    for (size_t x = 0; x < rowbytes; ++x)
                        dst[x] = (u8)(in_row[x] + (x >= bpp ? dst[x - bpp] : 0));
                    break;
                case 2:
                    for (size_t x = 0; x < rowbytes; ++x)
                        dst[x] = (u8)(in_row[x] + (up ? up[x] : 0));
                    break;
                case 3:
                    for (size_t x = 0; x < rowbytes; ++x)
                    {
                        const u32 prev = x >= bpp ? dst[x - bpp] : 0;
                        const u32 u = up ? up[x] : 0;
                        dst[x] = (u8)(in_row[x] + (prev + u) / 2);
                    }
                    break;
                case 4:
                    for (size_t x = 0; x < rowbytes; ++x)
                    {
                        const u8 a = x >= bpp ? dst[x - bpp] : 0;
                        const u8 b2 = up ? up[x] : 0;
                        const u8 c = (up && x >= bpp) ? up[x - bpp] : 0;
                        dst[x] = (u8)(in_row[x] + paeth(a, b2, c));
                    }
                    break;
                default:
                    set_err(err, "unknown PNG row filter");
                    return false;
                }
            }

            // ── 转 RGBA8 ──
            out.w = w;
            out.h = h;
            out.rgba.assign((size_t)w * h * 4, 0);

            auto put = [&](u32 x, u32 y, u8 r, u8 g, u8 b2, u8 a) {
                u8 *o = &out.rgba[((size_t)y * w + x) * 4];
                o[0] = r;
                o[1] = g;
                o[2] = b2;
                o[3] = a;
            };

            // 16 位通道：取高 8 位。这是**有损的**（写在这里而不是假装无损）：
            // fox 的纹理层是 RGBA8，16 位 PNG 只在比特深图像里出现，而把
            // 16 位整条链路带进来只为了不丢几个不可见的电平不值得。
            auto scale_to_8 = [](u32 v, int d) -> u8 {
                if (d == 8) return (u8)v;
                if (d == 16) return (u8)(v >> 8);
                return (u8)((v * 255u + ((1u << d) - 1u) / 2u) / ((1u << d) - 1u));
            };

            if (ctype == 3)
            {
                const u32 ncol = (u32)(plte.size() / 3);
                for (u32 y = 0; y < h; ++y)
                {
                    const u8 *row = rows.data() + (size_t)y * rowbytes;
                    for (u32 x = 0; x < w; ++x)
                    {
                        const u32 idx = depth == 8 ? row[x] : sub_sample(row, x, depth);
                        if (idx >= ncol)
                        {
                            set_err(err, "palette index out of range");
                            return false;
                        }
                        const u8 a = idx < trns.size() ? trns[idx] : (u8)255;
                        put(x, y, plte[idx * 3], plte[idx * 3 + 1], plte[idx * 3 + 2], a);
                    }
                }
                return true;
            }

            const size_t sample_bytes = (size_t)depth / 8;
            auto sample_at = [&](const u8 *row, u32 x, u32 c) -> u32 {
                if (depth < 8) return sub_sample(row, x, depth);
                const size_t o = (size_t)x * (size_t)ch * sample_bytes + (size_t)c * sample_bytes;
                return depth == 16 ? (((u32)row[o] << 8) | row[o + 1]) : row[o];
            };

            // tRNS：灰度比一个值、RGB 比三个值（只有"恰好相等"才透明，这是
            // 规范行为，不是近似）。
            const bool has_key = !trns.empty();
            u32 key_g = 0, key_r = 0, key_gb = 0, key_b = 0;
            if (has_key)
            {
                if (ctype == 0 && trns.size() >= 2) key_g = ((u32)trns[0] << 8) | trns[1];
                else if (ctype == 2 && trns.size() >= 6)
                {
                    key_r = ((u32)trns[0] << 8) | trns[1];
                    key_gb = ((u32)trns[2] << 8) | trns[3];
                    key_b = ((u32)trns[4] << 8) | trns[5];
                }
            }

            for (u32 y = 0; y < h; ++y)
            {
                const u8 *row = rows.data() + (size_t)y * rowbytes;
                for (u32 x = 0; x < w; ++x)
                {
                    if (ctype == 0)
                    {
                        const u32 v = sample_at(row, x, 0);
                        const u8 a = (has_key && v == key_g) ? 0 : 255;
                        const u8 g = scale_to_8(v, depth);
                        put(x, y, g, g, g, a);
                    }
                    else if (ctype == 2)
                    {
                        const u32 r = sample_at(row, x, 0), g = sample_at(row, x, 1),
                                  b2 = sample_at(row, x, 2);
                        const u8 a = (has_key && r == key_r && g == key_gb && b2 == key_b) ? 0 : 255;
                        put(x, y, scale_to_8(r, depth), scale_to_8(g, depth),
                            scale_to_8(b2, depth), a);
                    }
                    else if (ctype == 4)
                    {
                        const u32 g = sample_at(row, x, 0), a = sample_at(row, x, 1);
                        const u8 g8 = scale_to_8(g, depth);
                        put(x, y, g8, g8, g8, scale_to_8(a, depth));
                    }
                    else // ctype == 6
                    {
                        put(x, y, scale_to_8(sample_at(row, x, 0), depth),
                            scale_to_8(sample_at(row, x, 1), depth),
                            scale_to_8(sample_at(row, x, 2), depth),
                            scale_to_8(sample_at(row, x, 3), depth));
                    }
                }
            }
            return true;
        }

        // ── mipmap 链 ────────────────────────────────────────────────────

        MipChain build_mips(u32 w, u32 h, const u8 *rgba)
        {
            MipChain m;
            if (!w || !h || !rgba) return m;
            m.w = w;
            m.h = h;
            m.levels.emplace_back(rgba, rgba + (size_t)w * h * 4);

            u32 cw = w, ch2 = h;
            while (cw > 1 || ch2 > 1)
            {
                const u32 nw = cw > 1 ? cw / 2 : 1;
                const u32 nh = ch2 > 1 ? ch2 / 2 : 1;
                const std::vector<u8> &src = m.levels.back();
                std::vector<u8> dst((size_t)nw * nh * 4);

                for (u32 y = 0; y < nh; ++y)
                    for (u32 x = 0; x < nw; ++x)
                    {
                        // 2x2 box，奇数边时右侧/下侧钳位（边缘复制）。
                        const u32 x0 = x * 2 < cw ? x * 2 : cw - 1;
                        const u32 x1 = (x * 2 + 1) < cw ? x * 2 + 1 : cw - 1;
                        const u32 y0 = y * 2 < ch2 ? y * 2 : ch2 - 1;
                        const u32 y1 = (y * 2 + 1) < ch2 ? y * 2 + 1 : ch2 - 1;

                        u32 sa = 0, sr = 0, sg = 0, sb = 0;
                        const u32 xs[4] = {x0, x1, x0, x1};
                        const u32 ys[4] = {y0, y0, y1, y1};
                        for (int k = 0; k < 4; ++k)
                        {
                            const u8 *s = &src[((size_t)ys[k] * cw + xs[k]) * 4];
                            // 预乘后求和：直通 alpha 直接平均会把透明像素的
                            // 颜色（通常是很黑的 0）混进边缘。
                            const u32 a = s[3];
                            sa += a;
                            sr += (u32)s[0] * a;
                            sg += (u32)s[1] * a;
                            sb += (u32)s[2] * a;
                        }
                        const u32 a_avg = (sa + 2) / 4;
                        u8 *d = &dst[((size_t)y * nw + x) * 4];
                        d[3] = (u8)a_avg;
                        if (a_avg == 0)
                        {
                            d[0] = d[1] = d[2] = 0;
                        }
                        else
                        {
                            const u32 pr = (sr + 2) / 4, pg = (sg + 2) / 4, pb = (sb + 2) / 4;
                            d[0] = (u8)((pr + a_avg / 2) / a_avg);
                            d[1] = (u8)((pg + a_avg / 2) / a_avg);
                            d[2] = (u8)((pb + a_avg / 2) / a_avg);
                        }
                    }

                m.levels.push_back(std::move(dst));
                cw = nw;
                ch2 = nh;
            }
            return m;
        }

        namespace
        {
            inline u8 to_u8(f32 v)
            {
                if (v <= 0.0f) return 0;
                if (v >= 255.0f) return 255;
                return (u8)(v + 0.5f);
            }
        } // namespace

        void sample_bilinear(const MipChain &m, u32 level, f32 u, f32 v, u8 out[4])
        {
            if (m.levels.empty())
            {
                out[0] = out[1] = out[2] = out[3] = 0;
                return;
            }
            if (level >= m.levels.size()) level = (u32)m.levels.size() - 1;
            u32 lw = m.w, lh = m.h;
            for (u32 i = 0; i < level; ++i)
            {
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
            const std::vector<u8> &px = m.levels[level];

            // 中心对齐：texel 中心在 (i + 0.5) / size。边缘钳位，与
            // VkBackend 的采样器（CLAMP_TO_EDGE + LINEAR）同一规则。
            const f32 fx = u * (f32)lw - 0.5f;
            const f32 fy = v * (f32)lh - 0.5f;
            const i32 x0 = (i32)std::floor(fx), y0 = (i32)std::floor(fy);
            const f32 tx = fx - (f32)x0, ty = fy - (f32)y0;
            auto clampi = [](i32 v2, i32 hi) { return v2 < 0 ? 0 : (v2 > hi ? hi : v2); };
            const i32 xa = clampi(x0, (i32)lw - 1), xb = clampi(x0 + 1, (i32)lw - 1);
            const i32 ya = clampi(y0, (i32)lh - 1), yb = clampi(y0 + 1, (i32)lh - 1);

            const u8 *p00 = &px[((size_t)ya * lw + xa) * 4];
            const u8 *p10 = &px[((size_t)ya * lw + xb) * 4];
            const u8 *p01 = &px[((size_t)yb * lw + xa) * 4];
            const u8 *p11 = &px[((size_t)yb * lw + xb) * 4];
            for (int k = 0; k < 4; ++k)
            {
                const f32 a = p00[k] + (p10[k] - p00[k]) * tx;
                const f32 b = p01[k] + (p11[k] - p01[k]) * tx;
                out[k] = to_u8(a + (b - a) * ty);
            }
        }

        void sample_trilinear(const MipChain &m, f32 u, f32 v, f32 lod, u8 out[4])
        {
            if (m.levels.empty())
            {
                out[0] = out[1] = out[2] = out[3] = 0;
                return;
            }
            const f32 last = (f32)(m.levels.size() - 1);
            if (lod <= 0.0f)
            {
                sample_bilinear(m, 0, u, v, out);
                return;
            }
            if (lod >= last)
            {
                sample_bilinear(m, (u32)last, u, v, out);
                return;
            }
            const u32 l0 = (u32)lod;
            const f32 f = lod - (f32)l0;
            u8 a[4], b[4];
            sample_bilinear(m, l0, u, v, a);
            sample_bilinear(m, l0 + 1, u, v, b);
            for (int k = 0; k < 4; ++k) out[k] = to_u8((f32)a[k] + ((f32)b[k] - (f32)a[k]) * f);
        }

        f32 mip_lod(f32 texels_per_pixel)
        {
            if (texels_per_pixel <= 1.0f) return 0.0f;
            return (f32)(std::log2((f64)texels_per_pixel));
        }

        std::vector<u8> zlib_store(const u8 *src, size_t n)
        {
            std::vector<u8> out;
            out.reserve(n + n / 65535 * 5 + 16);
            out.push_back(0x78); // CMF: CM=8, CINFO=7（32K 窗口）
            out.push_back(0x01); // FLG: FCHECK 使 (0x78<<8|FLG) % 31 == 0, 无字典
            size_t off = 0;
            do
            {
                const size_t chunk = (n - off) > 65535 ? 65535 : (n - off);
                const bool last = off + chunk >= n;
                out.push_back(last ? 1 : 0);
                const u16 len = (u16)chunk;
                const u16 nlen = (u16)~len;
                out.push_back((u8)(len & 0xFF));
                out.push_back((u8)(len >> 8));
                out.push_back((u8)(nlen & 0xFF));
                out.push_back((u8)(nlen >> 8));
                out.insert(out.end(), src + off, src + off + chunk);
                off += chunk;
            } while (off < n);

            const u32 ad = adler32_bytes(src, n);
            out.push_back((u8)(ad >> 24));
            out.push_back((u8)(ad >> 16));
            out.push_back((u8)(ad >> 8));
            out.push_back((u8)ad);
            return out;
        }
    } // namespace img
} // namespace fox
