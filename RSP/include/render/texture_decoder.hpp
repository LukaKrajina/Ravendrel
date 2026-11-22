#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include "hardware/observability.hpp"
#include "scene.hpp"
#include "jpeg_decoder.hpp"

#ifdef RVDLRSP_USE_STB_IMAGE
#include "stb_image.h"
#endif

namespace rvdlrsp::render {

    class BitReader {
    private:
        const uint8_t *data_;
        size_t size_;
        size_t byte_pos_ = 0;
        uint32_t bit_buf_ = 0;
        int bit_count_ = 0;

    public:
        BitReader(const uint8_t *d, size_t n) : data_(d), size_(n) {}

        uint32_t read_bits(int n) {
            while (bit_count_ < n) {
                if (byte_pos_ < size_) {
                    bit_buf_ |= static_cast<uint32_t>(data_[byte_pos_++]) << bit_count_;
                    bit_count_ += 8;
                } else {
                    bit_count_ = n;
                    break;
                }
            }
            uint32_t mask = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
            uint32_t val = bit_buf_ & mask;
            bit_buf_ >>= n;
            bit_count_ -= n;
            return val;
        }

        void align_to_byte() {
            bit_buf_ = 0;
            bit_count_ = 0;
        }
    };

    struct LenDistEntry {
        int base;
        int extra;
    };

    inline const LenDistEntry &length_entry(int code) {
        static const LenDistEntry table[29] = {
            {3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},
            {11,1},{13,1},{15,1},{17,1},
            {19,2},{23,2},{27,2},{31,2},
            {35,3},{43,3},{51,3},{59,3},
            {67,4},{83,4},{99,4},{115,4},
            {131,5},{163,5},{195,5},{227,5},
            {258,0}
        };
        return table[code];
    }

    inline const LenDistEntry &dist_entry(int code) {
        static const LenDistEntry table[30] = {
            {1,0},{2,0},{3,0},{4,0},
            {5,1},{7,1},
            {9,2},{13,2},
            {17,3},{25,3},
            {33,4},{49,4},
            {65,5},{97,5},
            {129,6},{193,6},
            {257,7},{385,7},
            {513,8},{769,8},
            {1025,9},{1537,9},
            {2049,10},{3073,10},
            {4097,11},{6145,11},
            {8193,12},{12289,12},
            {16385,13},{24577,13}
        };
        return table[code];
    }

    inline uint32_t read_fixed_litlen(BitReader &br) {
        uint32_t c7 = br.read_bits(7);
        if (c7 < 0x18) return 256 + c7;
        uint32_t c8 = c7 | (br.read_bits(1) << 7);
        if (c8 >= 0x30 && c8 <= 0xBF) return c8 - 0x30;
        if (c8 >= 0xC0 && c8 <= 0xC7) return c8 - 0xC0 + 280;
        uint32_t c9 = c8 | (br.read_bits(1) << 8);
        return c9 - 0x190 + 144;
    }

    inline std::vector<uint8_t> inflate_zlib(const std::vector<uint8_t> &data) {
        if (data.size() < 6) throw std::runtime_error("PNG: zlib data too short");
        BitReader br(data.data() + 2, data.size() - 2);
        std::vector<uint8_t> out;

        while (true) {
            uint32_t bfinal = br.read_bits(1);
            uint32_t btype = br.read_bits(2);

            if (btype == 0) {
                br.align_to_byte();
                uint32_t len = br.read_bits(8) | (br.read_bits(8) << 8);
                br.read_bits(16);
                for (uint32_t i = 0; i < len; ++i)
                    out.push_back(static_cast<uint8_t>(br.read_bits(8)));
            } else if (btype == 1) {
                while (true) {
                    uint32_t sym = read_fixed_litlen(br);
                    if (sym < 256) {
                        out.push_back(static_cast<uint8_t>(sym));
                    } else if (sym == 256) {
                        break;
                    } else {
                        int li = static_cast<int>(sym) - 257;
                        const auto &le = length_entry(li);
                        int length = le.base + static_cast<int>(br.read_bits(le.extra));
                        uint32_t dsym = br.read_bits(5);
                        const auto &de = dist_entry(static_cast<int>(dsym));
                        int dist = de.base + static_cast<int>(br.read_bits(de.extra));
                        size_t start = out.size() - static_cast<size_t>(dist);
                        for (int i = 0; i < length; ++i)
                            out.push_back(out[start + i]);
                    }
                }
            } else if (btype == 2) {
                throw std::runtime_error("PNG: dynamic Huffman not supported");
            } else {
                throw std::runtime_error("PNG: invalid block type");
            }

            if (bfinal) break;
        }
        return out;
    }

    inline uint8_t paeth(int a, int b, int c) {
        int p = a + b - c;
        int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
        if (pb <= pc) return static_cast<uint8_t>(b);
        return static_cast<uint8_t>(c);
    }

    inline void unfilter(std::vector<uint8_t> &raw, int width, int height, int bpp) {
        int stride = width * bpp;
        std::vector<uint8_t> out(raw.size());
        for (int y = 0; y < height; ++y) {
            size_t row = static_cast<size_t>(y) * (stride + 1);
            uint8_t filter = raw[row];
            const uint8_t *cur = raw.data() + row + 1;
            uint8_t *dst = out.data() + static_cast<size_t>(y) * stride;
            const uint8_t *prev = (y > 0) ? out.data() + static_cast<size_t>(y - 1) * stride : nullptr;

            for (int x = 0; x < stride; ++x) {
                int a = (x >= bpp) ? dst[x - bpp] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
                int val = cur[x];
                switch (filter) {
                case 0: break;
                case 1: val += a; break;
                case 2: val += b; break;
                case 3: val += (a + b) / 2; break;
                case 4: val += paeth(a, b, c); break;
                default: throw std::runtime_error("PNG: unknown filter");
                }
                dst[x] = static_cast<uint8_t>(val & 0xFF);
            }
        }
        raw.swap(out);
    }

    inline DecodedImage decode_png(const std::vector<uint8_t> &data) {
        DecodedImage img;
        static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        if (data.size() < 8 || std::memcmp(data.data(), sig, 8) != 0) {
            throw std::runtime_error("PNG: invalid signature");
        }

        int width = 0, height = 0, bit_depth = 0, color_type = 0;
        std::vector<uint8_t> idat;

        size_t off = 8;
        while (off + 8 <= data.size()) {
            uint32_t len;
            std::memcpy(&len, data.data() + off, 4);
            len = ((len & 0xFF) << 24) | ((len & 0xFF00) << 8) | ((len >> 8) & 0xFF00) | ((len >> 24) & 0xFF);
            char type[5] = {0};
            std::memcpy(type, data.data() + off + 4, 4);
            off += 8;
            if (off + len > data.size()) throw std::runtime_error("PNG: truncated chunk");

            if (std::strcmp(type, "IHDR") == 0) {
                width = (data[off] << 24) | (data[off+1] << 16) | (data[off+2] << 8) | data[off+3];
                height = (data[off+4] << 24) | (data[off+5] << 16) | (data[off+6] << 8) | data[off+7];
                bit_depth = data[off+8];
                color_type = data[off+9];
            } else if (std::strcmp(type, "IDAT") == 0) {
                idat.insert(idat.end(), data.begin() + off, data.begin() + off + len);
            } else if (std::strcmp(type, "IEND") == 0) {
                break;
            }
            off += len + 4;
        }

        if (bit_depth != 8 || idat.empty()) {
            throw std::runtime_error("PNG: only 8-bit depth supported");
        }

        int channels;
        switch (color_type) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: throw std::runtime_error("PNG: unsupported color type (indexed not supported)");
        }

        std::vector<uint8_t> raw = inflate_zlib(idat);

        int bpp = channels;
        unfilter(raw, width, height, bpp);

        img.width = width;
        img.height = height;
        img.pixels.resize(static_cast<size_t>(width) * height * 4);

        for (int y = 0; y < height; ++y) {
            const uint8_t *src = raw.data() + static_cast<size_t>(y) * width * channels;
            uint8_t *dst = img.pixels.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                switch (color_type) {
                case 0:
                    dst[0] = dst[1] = dst[2] = src[x];
                    dst[3] = 255;
                    break;
                case 2:
                    dst[0] = src[x*3+0];
                    dst[1] = src[x*3+1];
                    dst[2] = src[x*3+2];
                    dst[3] = 255;
                    break;
                case 4:
                    dst[0] = dst[1] = dst[2] = src[x*2+0];
                    dst[3] = src[x*2+1];
                    break;
                case 6:
                    dst[0] = src[x*4+0];
                    dst[1] = src[x*4+1];
                    dst[2] = src[x*4+2];
                    dst[3] = src[x*4+3];
                    break;
                }
                dst += 4;
            }
        }

        img.valid = true;
        return img;
    }

    inline DecodedImage decode_jpeg(const std::vector<uint8_t> &data) {
        return decode_jpeg_full(data);
    }

#ifdef RVDLRSP_USE_STB_IMAGE
    inline DecodedImage decode_stb(const std::vector<uint8_t> &data) {
        DecodedImage img;
        int w = 0, h = 0, comp = 0;
        unsigned char *pixels = ::stbi_load_from_memory(
            data.data(), static_cast<int>(data.size()), &w, &h, &comp, 4);
        if (!pixels) {
            RVDLRSP_ERROR("render") << "stb_image failed to decode texture.";
            return img;
        }
        img.width = w;
        img.height = h;
        img.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        ::stbi_image_free(pixels);
        img.valid = true;
        return img;
    }
#endif

    class TextureDecoder {
    public:
        static DecodedImage decode(const Texture &tex) {
            if (!tex.valid || tex.data.empty()) return {};

#ifdef RVDLRSP_USE_STB_IMAGE
            return decode_stb(tex.data);
#else
            if (tex.mime == "image/png") return decode_png(tex.data);
            if (tex.mime == "image/jpeg" || tex.mime == "image/jpg")
                return decode_jpeg(tex.data);
            if (tex.data.size() > 4 && tex.data[0] == 0x89)
                return decode_png(tex.data);
            if (tex.data.size() > 2 && tex.data[0] == 0xFF && tex.data[1] == 0xD8)
                return decode_jpeg(tex.data);
            return {};
#endif
        }
    };
}