// ============================================================
// fox_png_test.cpp —— 自写 inflate / PNG 解码 / mipmap 链 / 三线性采样的证据
//
// 这一层的正确性不靠"打开一张图看着对"，靠三类可以自动判定的命题：
//
//   · zlib 自己产出的流解得回原字节，且 Adler-32 是第二道锁（改一个字节
//     也必须失败，而不是"大概对"）；
//   · RFC1951 的三种块类型**都被走到** —— InflateStats 会计数，而不是靠
//     "我觉得覆盖了"；LZ77 的 32768 窗口边界用手写的固定 Huffman 流精确
//     构造（zlib 不会为了测试恰好用上距离 32768，更不会产出越界的 32769）；
//   · 非法流 / 非法 PNG 必须失败，而且要失败在**正确的原因**上：越界回溯、
//     保留块类型、块 CRC、Adam7、未知行滤波器…… 一个"什么都解不出来"的
//     解码器会通过"必须失败"的检查，所以每一条都断言了错误文本。
//
// mipmap 与三线性采样的判据是**行为**而不是实现：texel 中心 ⇒ 该 texel
// 原值；最高 lod ⇒ 整图平均（缩小不再走样）；越界坐标 ⇒ 钳到边缘。
//
// 编译（无需窗口 / GPU / Vulkan）：
//   clang++ -std=c++20 -O2 -I runtime/include/ciUd/fox -I runtime/include \
//       runtime/tests/fox_png_test.cpp <fox .cpp 列表> -o fox_png_test
// CMake 目标 fox_png_test 链接 fox_core（[8] 需要 Context）。
// ============================================================

#include "image/png.hpp"
#include "core/context.hpp"
#include "fox_png_vectors.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fox;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        if (ok)
        {
            std::printf("  ok   %s\n", what);
            return;
        }
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }

    bool contains(const std::string &hay, const char *needle)
    {
        return hay.find(needle) != std::string::npos;
    }

    /// 与 runtime/tests/gen_png_vectors.py 的 pattern() 必须逐字节一致。
    /// 两边是同一公式的两个副本 —— 这正是几十 KB 明文不进头文件的原因：
    /// 数据副本会漂移，公式只有一份可对照。
    std::vector<u8> gen_pattern(size_t n)
    {
        std::vector<u8> out(n);
        u32 x = 0x2545F491u;
        for (size_t i = 0; i < n; ++i)
        {
            x = x * 1103515245u + 12345u;
            out[i] = (u8)((x >> 17) & 0xFF);
        }
        return out;
    }

    std::vector<u8> ab_pattern(size_t n)
    {
        std::vector<u8> out(n);
        for (size_t i = 0; i < n; ++i) out[i] = (i % 2 == 0) ? 'A' : 'B';
        return out;
    }

    std::vector<u8> expected_plain(const png_vectors::InflateCase &c)
    {
        if (c.pattern == 1) return gen_pattern(c.plain.len);
        if (c.pattern == 2)
        {
            std::vector<u8> v = ab_pattern(c.plain.len - 3);
            v.push_back('A');
            v.push_back('B');
            v.push_back('A');
            return v;
        }
        if (c.pattern == 3)
        {
            const std::vector<u8> block = gen_pattern(4096);
            std::vector<u8> v;
            v.reserve(block.size() * 17);
            for (int i = 0; i < 17; ++i) v.insert(v.end(), block.begin(), block.end());
            return v;
        }
        return std::vector<u8>(c.plain.data, c.plain.data + c.plain.len);
    }

    bool same(const std::vector<u8> &a, const unsigned char *b, size_t n)
    {
        return a.size() == n && (n == 0 || std::memcmp(a.data(), b, n) == 0);
    }
} // namespace

int main()
{
    // ── [1] inflate：zlib 产出的流 + 手写的窗口边界流 ──────────────────
    std::printf("[1] inflate：已知向量（zlib 产出 + 手写）\n");
    {
        u32 n_stored = 0, n_fixed = 0, n_dynamic = 0;
        for (int i = 0; i < png_vectors::kInflateCount; ++i)
        {
            const png_vectors::InflateCase &c = png_vectors::kInflateCases[i];
            std::vector<u8> out;
            img::InflateStats st;
            std::string err;
            const bool ok = img::inflate_zlib(c.z.data, c.z.len, out, &st, &err);
            const std::vector<u8> expect = expected_plain(c);
            std::printf("       %-18s %6zu B → %7zu B  (stored %u, fixed %u, dynamic %u)\n",
                        c.name, c.z.len, out.size(), st.stored, st.fixed, st.dynamic);
            if (!ok)
            {
                std::printf("       ERR: %s\n", err.c_str());
                check(false, c.name);
                continue;
            }
            check(same(out, expect.data(), expect.size()), c.name);
            n_stored += st.stored;
            n_fixed += st.fixed;
            n_dynamic += st.dynamic;
        }
        std::printf("       块类型计数：stored %u, fixed %u, dynamic %u\n", n_stored, n_fixed,
                    n_dynamic);
        check(n_stored > 0, "覆盖 stored 块");
        check(n_fixed > 0, "覆盖 fixed Huffman 块");
        check(n_dynamic > 0, "覆盖 dynamic Huffman 块");
    }

    // ── [2] inflate 负例：必须失败，且原因正确 ─────────────────────────
    std::printf("[2] inflate 负例：非法流必须失败在正确的原因上\n");
    for (int i = 0; i < png_vectors::kInflateBadCount; ++i)
    {
        const png_vectors::BadCase &c = png_vectors::kInflateBadCases[i];
        std::vector<u8> out;
        std::string err;
        const bool ok = img::inflate_zlib(c.blob.data, c.blob.len, out, nullptr, &err);
        std::string what = std::string("拒绝 ") + c.name + "（";
        what += ok ? "竟然解开了" : err;
        what += "）";
        check(!ok && contains(err, c.err_substr), what.c_str());
    }

    // ── [3] zlib_store：与 zlib level 0 逐字节一致 ─────────────────────
    std::printf("[3] zlib_store（写入方向）：与 zlib level 0 逐字节对拍\n");
    for (int i = 0; i < png_vectors::kStoreCount; ++i)
    {
        const png_vectors::StoreCase &c = png_vectors::kStoreCases[i];
        const std::vector<u8> z = img::zlib_store(c.plain.data, c.plain.len);
        std::string what = std::string("zlib_store == zlib level 0: ") + c.name;
        check(same(z, c.z.data, c.z.len), what.c_str());
    }
    {
        const std::vector<u8> big = gen_pattern(70000);
        const std::vector<u8> z = img::zlib_store(big.data(), big.size());
        std::vector<u8> back;
        img::InflateStats st;
        const bool ok = img::inflate_zlib(z.data(), z.size(), back, &st);
        check(ok && back == big, "70 KB 自写流往返（多块 stored）");
        check(st.stored == 2, "70000 字节 = 2 个 stored 块（>65535 必须分包）");
    }

    // ── [4] PNG：颜色类型 / 位深 / 行滤波器 / tRNS ─────────────────────
    std::printf("[4] PNG 解码：颜色类型 0/2/3/4/6、位深 1..16、行滤波器 0..4\n");
    for (int i = 0; i < png_vectors::kPngCount; ++i)
    {
        const png_vectors::PngCase &c = png_vectors::kPngCases[i];
        img::Image img;
        std::string err;
        const bool ok = img::decode_png(c.png.data, c.png.len, img, &err);
        if (!ok)
        {
            std::string what = std::string("PNG ") + c.name + ": " + err;
            check(false, what.c_str());
            continue;
        }
        std::string what = std::string("PNG ") + c.name;
        check(img.w == c.w && img.h == c.h && same(img.rgba, c.rgba.data, c.rgba.len),
              what.c_str());
    }

    // ── [5] PNG 负例 ───────────────────────────────────────────────────
    std::printf("[5] PNG 负例：坏文件必须失败在正确的原因上\n");
    for (int i = 0; i < png_vectors::kPngBadCount; ++i)
    {
        const png_vectors::BadCase &c = png_vectors::kPngBadCases[i];
        img::Image img;
        std::string err;
        const bool ok = img::decode_png(c.blob.data, c.blob.len, img, &err);
        std::string what = std::string("拒绝 ") + c.name + "（";
        what += ok ? "竟然解开了" : err;
        what += "）";
        check(!ok && contains(err, c.err_substr), what.c_str());
    }

    // ── [6] mipmap 链 ──────────────────────────────────────────────────
    std::printf("[6] mipmap 链\n");
    // 4x4，四个 2x2 色块：每一级都能手算验证。
    //   level 1 = 四个色块本身（块内是常量，box 平均必须精确还原，含 a=128/64）
    //   level 2 = 整图平均。手算（预乘平均，见 image/png.cpp build_mips）：
    //     sa = 255+128+255+64 = 702   → a = (702+2)/4 = 176
    //     sr = 255*255 + 0 + 0 + 255*64 = 81345 → r = 116
    //     sg = 0 + 255*128 + 0 + 255*64 = 48960 → g = 70
    //     sb = 0 + 0 + 255*255 + 0 = 65025       → b = 92
    static const u8 kBlocks[4][4] = {
        {255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 255}, {255, 255, 0, 64}};
    img::MipChain m;
    {
        std::vector<u8> px((size_t)4 * 4 * 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                std::memcpy(&px[((size_t)y * 4 + x) * 4], kBlocks[(y / 2) * 2 + x / 2], 4);
        m = img::build_mips(4, 4, px.data());
    }
    check(m.count() == 3 && m.levels[0].size() == 64 && m.levels[1].size() == 16 &&
              m.levels[2].size() == 4,
          "4x4 → 3 级（4x4, 2x2, 1x1）");
    {
        bool l1_ok = true;
        for (int i = 0; i < 4; ++i)
            if (std::memcmp(&m.levels[1][(size_t)i * 4], kBlocks[i], 4) != 0) l1_ok = false;
        check(l1_ok, "level 1 精确还原四个色块（含 a=128 / a=64 的半透明块）");
        static const u8 kMean[4] = {116, 70, 92, 176};
        check(std::memcmp(m.levels[2].data(), kMean, 4) == 0,
              "level 2 = 整图平均 (116,70,92,176)");
    }
    {
        // 奇数边长：右/下边缘复制；尺寸必须止于 1x1（不能出现 0 尺寸层）
        std::vector<u8> px((size_t)6 * 5 * 4);
        for (size_t i = 0; i < px.size(); ++i) px[i] = (u8)(i * 7 + 3);
        const img::MipChain m2 = img::build_mips(6, 5, px.data());
        check(m2.count() == 3 && m2.levels[1].size() == (size_t)3 * 2 * 4 &&
                  m2.levels[2].size() == 4,
              "6x5 → 6x5, 3x2, 1x1（奇数边钳位）");
        const img::MipChain m3 = img::build_mips(1, 1, px.data());
        check(m3.count() == 1, "1x1 只有一级");
    }

    // ── [7] 三线性采样 ─────────────────────────────────────────────────
    std::printf("[7] 三线性采样\n");
    {
        u8 o[4], o2[4];

        img::sample_trilinear(m, 0.125f, 0.125f, 0.0f, o);
        check(o[0] == 255 && o[1] == 0 && o[2] == 0 && o[3] == 255,
              "lod=0、texel 中心 ⇒ 该 texel 原值（不被邻居混色）");

        img::sample_trilinear(m, 0.5f, 0.9f, 99.0f, o);
        check(o[0] == 116 && o[1] == 70 && o[2] == 92 && o[3] == 176,
              "lod 越界钳到最后一级 = 整图平均（缩小不再走样）");

        img::sample_trilinear(m, -3.0f, -3.0f, 0.0f, o);
        img::sample_trilinear(m, 0.0f, 0.0f, 0.0f, o2);
        check(std::memcmp(o, o2, 4) == 0, "uv 越界钳到边缘（等于 (0,0)）");

        u8 a[4], b[4];
        img::sample_bilinear(m, 1, 0.375f, 0.375f, a);
        img::sample_bilinear(m, 2, 0.375f, 0.375f, b);
        img::sample_trilinear(m, 0.375f, 0.375f, 1.25f, o);
        bool lerp_ok = true;
        for (int k = 0; k < 4; ++k)
        {
            const f32 v = (f32)a[k] + ((f32)b[k] - (f32)a[k]) * 0.25f;
            if (o[k] != (u8)(v + 0.5f)) lerp_ok = false;
        }
        check(lerp_ok, "小数 lod：两级双线性结果按小数部分插值");

        check(img::mip_lod(1.0f) == 0.0f && img::mip_lod(0.5f) == 0.0f &&
                  img::mip_lod(2.0f) == 1.0f,
              "mip_lod：1:1 → 0，放大 → 0，缩小 2× → 1");
        const f32 l4 = img::mip_lod(4.0f);
        check(l4 > 1.99f && l4 < 2.01f, "mip_lod(4) = 2");
    }

    // ── [8] Context 接线：PNG 纹理 + LOD 采样 ──────────────────────────
    std::printf("[8] Context 接线（create_texture_png / sample_rgba_lod）\n");
    {
        const png_vectors::PngCase *blk = nullptr;
        for (int i = 0; i < png_vectors::kPngCount; ++i)
            if (std::strcmp(png_vectors::kPngCases[i].name, "blocks-4x4-mip") == 0)
                blk = &png_vectors::kPngCases[i];
        if (!blk)
        {
            check(false, "找到 4x4 色块向量");
        }
        else
        {
            Context &c = ctx();
            c.init("fox png test", 64, 64);

            const TexId tex = c.create_texture_png(blk->png.data, blk->png.len, "blocks");
            check(tex != TEX_NONE, "create_texture_png 成功");
            const Context::Texture *t = c.texture(tex);
            check(t && t->w == 4 && t->h == 4 && t->mip_levels() == 3, "纹理带 3 级 mip 链");

            u8 o[4];
            c.sample_rgba_lod(tex, 0.125f, 0.125f, 0.0f, o);
            check(o[0] == 255 && o[1] == 0 && o[2] == 0 && o[3] == 255,
                  "texture：lod 0 的 texel 原值");
            c.sample_rgba_lod(tex, 0.5f, 0.5f, 9.0f, o);
            check(o[0] == 116 && o[1] == 70 && o[2] == 92 && o[3] == 176,
                  "texture：高 lod = 整图平均");

            // 无 mip 的纹理必须仍然是原来的最近邻 —— 既有内容的像素一个都不许动
            const TexId plain = c.create_texture(4, 4, blk->rgba.data);
            u8 nn[4];
            c.sample_rgba_lod(plain, 0.3f, 0.7f, 3.0f, nn);
            const u8 *s = c.sample_rgba(plain, 0.3f, 0.7f);
            check(s && std::memcmp(nn, s, 4) == 0, "无 mip 的纹理仍走最近邻（像素不变）");
            check(c.texture(plain) && c.texture(plain)->mip_levels() == 1,
                  "create_texture 不建 mip 链（字形图集是热路径）");

            // update_texture 必须让旧 mip 失效：留下旧级 = 把旧像素混进新图
            std::vector<u8> other((size_t)4 * 4 * 4, 7);
            check(c.update_texture(tex, 4, 4, other.data()), "update_texture 成功");
            check(c.texture(tex) && c.texture(tex)->mip_levels() == 1,
                  "update 后 mip 链被丢弃");
            c.sample_rgba_lod(tex, 0.5f, 0.5f, 5.0f, o);
            check(o[0] == 7, "update 后高 lod 也从新像素取样");

            check(c.create_texture_png(reinterpret_cast<const u8 *>("nope"), 4) == TEX_NONE,
                  "坏输入返回 TEX_NONE（打印原因，不交空白纹理）");
        }
    }

    std::printf("\n%s（失败 %d 项）\n",
                g_failures == 0 ? "FOX-PNG: 全部通过" : "FOX-PNG: 失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}