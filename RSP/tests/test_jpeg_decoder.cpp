#include "test_framework.hpp"
#include "render/jpeg_decoder.hpp"
#include "render/texture_decoder.hpp"
#include <vector>
#include <cstdint>
#include <exception>
#include <iostream>

using namespace rvdlrsp::render;

static std::vector<uint8_t> make_grayscale_jpeg(uint8_t gray) {
    std::vector<uint8_t> jpeg;
    auto push = [&](uint8_t b) { jpeg.push_back(b); };
    auto push16 = [&](uint16_t v) { push(v >> 8); push(v & 0xFF); };

    push(0xFF); push(0xD8);

    {
        std::vector<uint8_t> data;
        data.push_back(0x00);
        for (int i = 0; i < 64; ++i) data.push_back(1);
        push(0xFF); push(0xDB);
        push16(static_cast<uint16_t>(data.size() + 2));
        for (uint8_t b : data) push(b);
    }

    {
        push(0xFF); push(0xC0);
        push16(8 + 3);
        push(8);
        push16(1);
        push16(1);
        push(1);
        push(1);
        push(0x11);
        push(0);
    }

    {
        std::vector<uint8_t> data;
        data.push_back(0x00);
        const uint8_t bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
        for (int i = 0; i < 16; ++i) data.push_back(bits[i]);
        const uint8_t vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
        for (int i = 0; i < 12; ++i) data.push_back(vals[i]);

        push(0xFF); push(0xC4);
        push16(static_cast<uint16_t>(data.size() + 2));
        for (uint8_t b : data) push(b);
    }

    {
        std::vector<uint8_t> data;
        data.push_back(0x10);
        const uint8_t bits[16] = {0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0};
        for (int i = 0; i < 16; ++i) data.push_back(bits[i]);
        data.push_back(0x00);

        push(0xFF); push(0xC4);
        push16(static_cast<uint16_t>(data.size() + 2));
        for (uint8_t b : data) push(b);
    }

    {
        push(0xFF); push(0xDA);
        push16(8);
        push(1);
        push(1);
        push(0x00);
        push(0);
        push(63);
        push(0);
    }

    int diff = gray;
    int s = 0;
    int abs_diff = diff < 0 ? -diff : diff;
    while ((1 << s) <= abs_diff) ++s;

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

    struct { int len; int code; } dc_codes[12] = {
        {2,0},{3,2},{3,3},{3,4},{3,5},{3,6},{4,14},{5,30},{6,62},{7,126},{8,254},{9,510}
    };
    if (s > 11) s = 11;
    emit_bits(entropy, bitpos, dc_codes[s].code, dc_codes[s].len);

    if (s > 0) {
        int value = (diff >= 0) ? diff : (diff + (1 << s) - 1);
        emit_bits(entropy, bitpos, static_cast<uint32_t>(value), s);
    }

    emit_bits(entropy, bitpos, 0x0, 4);

    if (bitpos % 8 != 0) {
        emit_bits(entropy, bitpos, 0, 8 - (bitpos % 8));
    }
    if (!entropy.empty() && entropy.back() == 0xFF) entropy.push_back(0x00);

    for (uint8_t b : entropy) push(b);

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