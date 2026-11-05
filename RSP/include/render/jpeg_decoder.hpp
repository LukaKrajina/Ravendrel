#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "scene.hpp"

namespace rvdlrsp::render
{

    class JpegDecoder
    {
        const uint8_t *data = nullptr;
        size_t size = 0;
        size_t pos = 0;

        uint8_t byte()
        {
            if (pos >= size)
                throw std::runtime_error("JPEG: unexpected EOF");
            return data[pos++];
        }
        uint16_t be16() { return static_cast<uint16_t>((byte() << 8) | byte()); }
        void skip(size_t n)
        {
            pos += n;
            if (pos > size)
                throw std::runtime_error("JPEG: skip EOF");
        }

        // 位读取（0xFF00 → 0xFF 填充）
        uint32_t bit_buf = 0;
        int bit_count = 0;

        int read_bits(int n)
        {
            while (bit_count < n)
            {
                uint8_t b = byte();
                bit_buf = (bit_buf << 8) | b;
                bit_count += 8;
                if (b == 0xFF)
                {
                    uint8_t next = byte();
                    if (next != 0x00)
                    {
                        pos--;
                        break;
                    }
                }
            }
            int val = static_cast<int>((bit_buf >> (bit_count - n)) & ((1u << n) - 1u));
            bit_count -= n;
            return val;
        }

        // Huffman 表
        struct HuffTable
        {
            int mincode[16] = {0};
            int maxcode[16] = {0};
            int valptr[16] = {0};
            uint8_t huffval[256] = {0};
            int total = 0;
        };
        HuffTable dc[4], ac[4];

        void read_dht()
        {
            int len = be16() - 2;
            while (len > 0)
            {
                uint8_t info = byte();
                --len;
                int tc = (info >> 4) & 1;
                int th = info & 0x0F;
                if (th >= 4)
                    throw std::runtime_error("JPEG: bad Huffman id");
                HuffTable &t = tc ? ac[th] : dc[th];

                int total = 0;
                for (int i = 0; i < 16; ++i)
                {
                    t.valptr[i] = total;
                    total += byte();
                    --len;
                }
                if (total > 256)
                    throw std::runtime_error("JPEG: Huffman too large");
                t.total = total;
                for (int i = 0; i < total; ++i)
                {
                    t.huffval[i] = byte();
                    --len;
                }

                // 构建 mincode/maxcode
                int code = 0;
                for (int i = 0; i < 16; ++i)
                {
                    int cnt = (i == 15) ? (total - t.valptr[15]) : (t.valptr[i + 1] - t.valptr[i]);
                    if (cnt > 0)
                    {
                        t.mincode[i] = code;
                        t.maxcode[i] = code + cnt - 1;
                    }
                    else
                    {
                        t.mincode[i] = -1;
                        t.maxcode[i] = -1;
                    }
                    code = (code + cnt) << 1;
                }
            }
        }

        int decode_symbol(const HuffTable &t)
        {
            int code = 0;
            for (int i = 0; i < 16; ++i)
            {
                code = (code << 1) | read_bits(1);
                if (t.mincode[i] >= 0 && code <= t.maxcode[i] && code >= t.mincode[i])
                {
                    int idx = t.valptr[i] + (code - t.mincode[i]);
                    if (idx >= t.total)
                        throw std::runtime_error("JPEG: Huffman idx");
                    return t.huffval[idx];
                }
            }
            throw std::runtime_error("JPEG: invalid Huffman code");
        }

        int receive(int s)
        {
            if (s == 0)
                return 0;
            int v = read_bits(s);
            if (v < (1 << (s - 1)))
                v -= (1 << s) - 1;
            return v;
        }

        // 量化表
        float qt[4][64];
        bool qt_ok[4] = {false, false, false, false};

        void read_dqt()
        {
            int len = be16() - 2;
            while (len > 0)
            {
                uint8_t info = byte();
                --len;
                int pq = (info >> 4) & 0x0F;
                int tq = info & 0x0F;
                if (tq >= 4)
                    throw std::runtime_error("JPEG: bad quant id");
                static const int zigzag[64] = {
                    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
                    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
                for (int i = 0; i < 64; ++i)
                {
                    int v = (pq == 0) ? byte() : static_cast<int>(be16());
                    len -= (pq == 0) ? 1 : 2;
                    qt[tq][zigzag[i]] = static_cast<float>(v);
                }
                qt_ok[tq] = true;
            }
        }

        // 帧头
        int width = 0, height = 0, components = 0;
        int h[3] = {1, 1, 1}, v[3] = {1, 1, 1};
        int qt_id[3] = {0, 0, 0};

        void read_sof()
        {
            int len = be16() - 2;
            byte(); // precision
            height = be16();
            width = be16();
            components = byte();
            len -= 6;
            if (components > 3)
                throw std::runtime_error("JPEG: >3 components unsupported");
            for (int i = 0; i < components; ++i)
            {
                uint8_t id = byte();
                uint8_t hv = byte();
                uint8_t q = byte();
                len -= 3;
                int idx = id - 1;
                if (idx < 0 || idx >= 3)
                    throw std::runtime_error("JPEG: bad component id");
                h[idx] = (hv >> 4) & 0x0F;
                v[idx] = hv & 0x0F;
                qt_id[idx] = q;
            }
            (void)len;
        }

        // 扫描头
        int dc_id[3] = {0, 0, 0}, ac_id[3] = {0, 0, 0};

        void read_sos()
        {
            int len = be16() - 2;
            int ns = byte();
            for (int i = 0; i < ns; ++i)
            {
                uint8_t id = byte();
                uint8_t tables = byte();
                int idx = id - 1;
                if (idx >= 0 && idx < 3)
                {
                    dc_id[idx] = (tables >> 4) & 0x0F;
                    ac_id[idx] = tables & 0x0F;
                }
            }
            byte();
            byte();
            byte(); // Ss, Se, AhAl
            (void)len;
        }

        // IDCT（直接实现）
        static void idct_8x8(float *blk)
        {
            float tmp[64];
            const float PI = 3.141592653589793f;
            for (int x = 0; x < 8; ++x)
                for (int y = 0; y < 8; ++y)
                {
                    float sum = 0;
                    for (int u = 0; u < 8; ++u)
                        for (int v = 0; v < 8; ++v)
                        {
                            float cu = (u == 0) ? 1.0f / std::sqrt(2.0f) : 1.0f;
                            float cv = (v == 0) ? 1.0f / std::sqrt(2.0f) : 1.0f;
                            sum += cu * cv * blk[v * 8 + u] *
                                   std::cos((2 * x + 1) * u * PI / 16.0f) *
                                   std::cos((2 * y + 1) * v * PI / 16.0f);
                        }
                    tmp[y * 8 + x] = sum * 0.25f;
                }
            for (int i = 0; i < 64; ++i)
                blk[i] = tmp[i];
        }

        void decode_block(float *out, const HuffTable &dt, const HuffTable &at, int qid)
        {
            float blk[64] = {0};
            int t = decode_symbol(dt);
            int diff = receive(t);
            blk[0] = static_cast<float>(diff);

            int k = 1;
            while (k < 64)
            {
                int rs = decode_symbol(at);
                int r = rs >> 4;
                int s = rs & 0x0F;
                if (s == 0)
                {
                    if (r == 15)
                    {
                        k += 16;
                        continue;
                    }
                    break;
                }
                k += r;
                if (k >= 64)
                    throw std::runtime_error("JPEG: AC overflow");
                blk[k++] = static_cast<float>(receive(s));
            }

            for (int i = 0; i < 64; ++i)
                blk[i] *= qt[qid][i];
            idct_8x8(blk);
            for (int i = 0; i < 64; ++i)
                out[i] = blk[i];
        }

        static uint8_t clampf(float x)
        {
            if (x < 0)
                return 0;
            if (x > 255)
                return 255;
            return static_cast<uint8_t>(x + 0.5f);
        }

        void decode_scan(std::vector<std::vector<float>> &channels)
        {
            int max_h = 1, max_v = 1;
            for (int i = 0; i < components; ++i)
            {
                if (h[i] > max_h)
                    max_h = h[i];
                if (v[i] > max_v)
                    max_v = v[i];
            }
            int mcu_w = (width + 8 * max_h - 1) / (8 * max_h);
            int mcu_h = (height + 8 * max_v - 1) / (8 * max_v);

            std::vector<int> bw(components), bh(components);
            for (int i = 0; i < components; ++i)
            {
                bw[i] = mcu_w * h[i];
                bh[i] = mcu_h * v[i];
                channels.push_back(std::vector<float>(static_cast<size_t>(bw[i]) * bh[i] * 64, 0.0f));
            }

            std::vector<int> dc_pred(components, 0);

            for (int my = 0; my < mcu_h; ++my)
                for (int mx = 0; mx < mcu_w; ++mx)
                    for (int c = 0; c < components; ++c)
                        for (int vy = 0; vy < v[c]; ++vy)
                            for (int vx = 0; vx < h[c]; ++vx)
                            {
                                float blk[64];
                                decode_block(blk, dc[dc_id[c]], ac[ac_id[c]], qt_id[c]);
                                blk[0] += static_cast<float>(dc_pred[c]);
                                dc_pred[c] = static_cast<int>(blk[0]);
                                int bx = mx * h[c] + vx;
                                int by = my * v[c] + vy;
                                size_t idx = (static_cast<size_t>(by) * bw[c] + bx) * 64;
                                for (int i = 0; i < 64; ++i)
                                    channels[c][idx + i] = blk[i];
                            }

            // 上采样到全分辨率
            for (int c = 0; c < components; ++c)
            {
                std::vector<float> full(static_cast<size_t>(width) * height);
                for (int y = 0; y < height; ++y)
                {
                    int sy = (y * v[c]) / max_v;
                    if (sy >= bh[c])
                        sy = bh[c] - 1;
                    for (int x = 0; x < width; ++x)
                    {
                        int sx = (x * h[c]) / max_h;
                        if (sx >= bw[c])
                            sx = bw[c] - 1;
                        size_t bidx = (static_cast<size_t>(sy) * bw[c] + sx) * 64;
                        full[static_cast<size_t>(y) * width + x] =
                            channels[c][bidx + (sy % 8) * 8 + (sx % 8)];
                    }
                }
                channels[c] = std::move(full);
            }
        }

    public:
        static DecodedImage decode(const std::vector<uint8_t> &bytes)
        {
            JpegDecoder d;
            d.data = bytes.data();
            d.size = bytes.size();
            return d.run();
        }

    private:
        DecodedImage run()
        {
            if (byte() != 0xFF || byte() != 0xD8)
                throw std::runtime_error("JPEG: bad SOI");

            std::vector<std::vector<float>> channels;
            DecodedImage img;

            while (pos < size)
            {
                uint8_t marker = byte();
                if (marker != 0xFF)
                    continue;
                while (pos < size && data[pos] == 0xFF)
                    pos++;
                uint8_t m = byte();

                if (m == 0xD9 || m == 0xDA)
                {
                    if (m == 0xDA)
                    {
                        read_sos();
                        decode_scan(channels);
                    }
                    break;
                }
                if (m == 0xC0 || m == 0xC1 || m == 0xC2)
                    read_sof();
                else if (m == 0xC4)
                    read_dht();
                else if (m == 0xDB)
                    read_dqt();
                else
                {
                    int len = be16();
                    skip(len - 2);
                }
            }

            img.width = width;
            img.height = height;
            img.pixels.assign(static_cast<size_t>(width) * height * 4, 255);

            if (components == 1)
            {
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                    {
                        float g = clampf(channels[0][static_cast<size_t>(y) * width + x]);
                        size_t idx = (static_cast<size_t>(y) * width + x) * 4;
                        img.pixels[idx] = img.pixels[idx + 1] = img.pixels[idx + 2] = g;
                    }
            }
            else if (components == 3)
            {
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                    {
                        size_t p = static_cast<size_t>(y) * width + x;
                        float Y = channels[0][p];
                        float Cb = channels[1][p] - 128.0f;
                        float Cr = channels[2][p] - 128.0f;
                        float r = Y + 1.402f * Cr;
                        float g = Y - 0.344136f * Cb - 0.714136f * Cr;
                        float b = Y + 1.772f * Cb;
                        size_t idx = p * 4;
                        img.pixels[idx] = clampf(r);
                        img.pixels[idx + 1] = clampf(g);
                        img.pixels[idx + 2] = clampf(b);
                    }
            }
            else
            {
                throw std::runtime_error("JPEG: unsupported components");
            }

            img.valid = true;
            return img;
        }
    };

    inline DecodedImage decode_jpeg_full(const std::vector<uint8_t> &data)
    {
        return JpegDecoder::decode(data);
    }
}