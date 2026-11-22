#pragma once
#include "core/types.hpp"
#include <string>
#include <vector>

namespace fox
{
    namespace img
    {

        struct InflateStats
        {
            u32 stored = 0;
            u32 fixed = 0;
            u32 dynamic = 0;
        };

        bool inflate_zlib(const u8 *src, size_t n, std::vector<u8> &out,
                          InflateStats *stats = nullptr, std::string *err = nullptr,
                          size_t max_out = (size_t)1 << 28);


        struct Image
        {
            u32 w = 0;
            u32 h = 0;
            std::vector<u8> rgba;
        };

        bool decode_png(const u8 *src, size_t n, Image &out, std::string *err = nullptr);


        struct MipChain
        {
            u32 w = 0;
            u32 h = 0;
            std::vector<std::vector<u8>> levels;
            u32 count() const { return (u32)levels.size(); }
        };

        MipChain build_mips(u32 w, u32 h, const u8 *rgba);

        void sample_bilinear(const MipChain &m, u32 level, f32 u, f32 v, u8 out[4]);

        void sample_trilinear(const MipChain &m, f32 u, f32 v, f32 lod, u8 out[4]);

        f32 mip_lod(f32 texels_per_pixel);

        std::vector<u8> zlib_store(const u8 *src, size_t n);
    }
}