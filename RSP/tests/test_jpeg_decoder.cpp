// JPEG 基线解码器单元测试
// 用程序构造最小基线 JPEG（灰度，1x1），验证解码正确性。
#include "test_framework.hpp"
#include "render/jpeg_decoder.hpp"
#include "render/texture_decoder.hpp"
#include <vector>
#include <cstdint>
#include <exception>
#include <iostream>

using namespace rvdlrsp::render;

// 构造最小 1x1 灰度基线 JPEG
// 使用标准灰度 Huffman 表 + 全 1 量化表，DC 系数编码为一个特定值。
static std::vector<uint8_t> make_grayscale_jpeg(uint8_t gray) {
    std::vector<uint8_t> jpeg;
    auto push = [&](uint8_t b) { jpeg.push_back(b); };
    auto push16 = [&](uint16_t v) { push(v >> 8); push(v & 0xFF); };

    // SOI
    push(0xFF); push(0xD8);

    // DQT：灰度量化表（全 1，简化）
    {
        std::vector<uint8_t> data;
        data.push_back(0x00); // Pq=0, Tq=0
        for (int i = 0; i < 64; ++i) data.push_back(1);
        push(0xFF); push(0xDB);
        push16(static_cast<uint16_t>(data.size() + 2));
        for (uint8_t b : data) push(b);
    }

    // SOF0：灰度，1x1
    {
        push(0xFF); push(0xC0);
        push16(8 + 3); // Lf = 8 + 3*Ns
        push(8);       // precision
        push16(1);     // height
        push16(1);     // width
        push(1);       // Ns=1
        push(1);       // component id=1
        push(0x11);    // H=1,V=1
        push(0);       // Tq=0
    }

    // DHT：标准灰度 DC 表
    {
        std::vector<uint8_t> data;
        data.push_back(0x00); // Tc=0(DC), Th=0
        // BITS：标准亮度 DC（12 个码，长度分布）
        const uint8_t bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
        for (int i = 0; i < 16; ++i) data.push_back(bits[i]);
        // HUFFVAL
        const uint8_t vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
        for (int i = 0; i < 12; ++i) data.push_back(vals[i]);

        push(0xFF); push(0xC4);
        push16(static_cast<uint16_t>(data.size() + 2));
        for (uint8_t b : data) push(b);
    }

    // DHT：AC 表（EOB 为 4 位码，长度 4 有 1 个符号）
    {
        std::vector<uint8_t> data;
        data.push_back(0x10); // Tc=1(AC), Th=0
        const uint8_t bits[16] = {0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0};
        for (int i = 0; i < 16; ++i) data.push_back(bits[i]);
        // 一个符号 0x00（EOB）
        data.push_back(0x00);

        push(0xFF); push(0xC4);
        push16(static_cast<uint16_t>(data.size() + 2));
        for (uint8_t b : data) push(b);
    }

    // SOS：灰度
    {
        push(0xFF); push(0xDA);
        push16(8);   // Ls = 6 + 2*Ns
        push(1);     // Ns=1
        push(1);     // component id=1
        push(0x00);  // DC=0, AC=0
        push(0);     // Ss
        push(63);    // Se
        push(0);     // AhAl
    }

    // 熵编码数据：DC diff = gray，AC 全 EOB。仅做结构验证，值编码从简。
    // 求 DC category s：
    int diff = gray;
    int s = 0;
    int abs_diff = diff < 0 ? -diff : diff;
    while ((1 << s) <= abs_diff) ++s;
    // 符号 = s（标准表符号 0..11 对应 category 0..11）
    // 位写入：符号（Huffman 编码）+ diff 的 s 位

    // 手写位流
    auto emit_bits = [&](std::vector<uint8_t> &bytes, int &bitpos, uint32_t code, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            int bit = (code >> i) & 1;
            if (bitpos % 8 == 0) bytes.push_back(0);
            bytes[bytes.size() - 1] |= static_cast<uint8_t>(bit << (7 - (bitpos % 8)));
            ++bitpos;
        }
    };

    std::vector<uint8_t> entropy;
    int bitpos = 0;

    // 标准亮度 DC 表编码：category s 对应 Huffman 码
    // category 0: "00", 1:"010", 2:"011", 3:"100", 4:"101", 5:"110", 6:"1110", ...
    // 直接使用标准码表
    struct { int len; int code; } dc_codes[12] = {
        {2,0},{3,2},{3,3},{3,4},{3,5},{3,6},{4,14},{5,30},{6,62},{7,126},{8,254},{9,510}
    };
    if (s > 11) s = 11;
    emit_bits(entropy, bitpos, dc_codes[s].code, dc_codes[s].len);

    // diff 的值（s 位）
    if (s > 0) {
        int value = (diff >= 0) ? diff : (diff + (1 << s) - 1);
        emit_bits(entropy, bitpos, static_cast<uint32_t>(value), s);
    }

    // AC：EOB（本测试 AC 表的 EOB 码为 4 位全 0，即 0000）
    emit_bits(entropy, bitpos, 0x0, 4); // 0000

    // 填充到字节边界 + 0xFF 标记
    if (bitpos % 8 != 0) {
        emit_bits(entropy, bitpos, 0, 8 - (bitpos % 8));
    }
    // 如果最后字节是 0xFF，需要填充 0x00
    if (!entropy.empty() && entropy.back() == 0xFF) entropy.push_back(0x00);

    for (uint8_t b : entropy) push(b);

    // EOI
    push(0xFF); push(0xD9);

    return jpeg;
}

QTEST(jpeg_decode_grayscale) {
    std::vector<uint8_t> jpeg = make_grayscale_jpeg(128);
    QCHECK(jpeg.size() > 20);

    try {
        DecodedImage img = decode_jpeg_full(jpeg);
        QCHECK(img.valid);
        QCHECK(img.width == 1);
        QCHECK(img.height == 1);
        QCHECK(img.pixels.size() == 4);
        QCHECK(img.pixels[0] == img.pixels[1]);
        QCHECK(img.pixels[1] == img.pixels[2]);
    } catch (const std::exception &e) {
        std::cerr << "  JPEG decode threw: " << e.what() << "\n";
        ++::qtest::failures();
    }
}

QTEST(jpeg_invalid_throws) {
    bool threw = false;
    try {
        decode_jpeg_full({1, 2, 3, 4});
    } catch (const std::exception &) {
        threw = true;
    }
    QCHECK(threw);
}
