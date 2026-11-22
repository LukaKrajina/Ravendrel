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
}

int main()
{
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

    std::printf("[6] mipmap 链\n");
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
        std::vector<u8> px((size_t)6 * 5 * 4);
        for (size_t i = 0; i < px.size(); ++i) px[i] = (u8)(i * 7 + 3);
        const img::MipChain m2 = img::build_mips(6, 5, px.data());
        check(m2.count() == 3 && m2.levels[1].size() == (size_t)3 * 2 * 4 &&
                  m2.levels[2].size() == 4,
              "6x5 → 6x5, 3x2, 1x1（奇数边钳位）");
        const img::MipChain m3 = img::build_mips(1, 1, px.data());
        check(m3.count() == 1, "1x1 只有一级");
    }

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

            const TexId plain = c.create_texture(4, 4, blk->rgba.data);
            u8 nn[4];
            c.sample_rgba_lod(plain, 0.3f, 0.7f, 3.0f, nn);
            const u8 *s = c.sample_rgba(plain, 0.3f, 0.7f);
            check(s && std::memcmp(nn, s, 4) == 0, "无 mip 的纹理仍走最近邻（像素不变）");
            check(c.texture(plain) && c.texture(plain)->mip_levels() == 1,
                  "create_texture 不建 mip 链（字形图集是热路径）");

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