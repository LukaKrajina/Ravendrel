// 纹理解码器（PNG）单元测试
#include "test_framework.hpp"
#include "render/texture_decoder.hpp"
#include <cstring>
#include <vector>
#include <cstdint>

using namespace rvdlrsp::render;

static void push_be32_wrapper(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

// 构造一个最小的 1x1 RGB PNG（stored deflate 块，不压缩）
static std::vector<uint8_t> make_minimal_png(int width, int height,
                                             const std::vector<uint8_t> &rgb_rows) {
    std::vector<uint8_t> png;
    auto push_be32 = [&](uint32_t v) {
        png.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        png.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        png.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        png.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push_chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        push_be32(static_cast<uint32_t>(data.size()));
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        // CRC（占位，解码器不校验）
        push_be32(0);
    };

    // PNG 签名
    const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    png.insert(png.end(), sig, sig + 8);

    // IHDR
    std::vector<uint8_t> ihdr;
    push_be32_wrapper(ihdr, width);
    ihdr.push_back(0); // height 高 3 字节（小尺寸）
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(static_cast<uint8_t>(height));
    ihdr.push_back(8);  // bit depth（位深）
    ihdr.push_back(2);  // color type RGB（颜色类型）
    ihdr.push_back(0);  // compression（压缩方式）
    ihdr.push_back(0);  // filter（滤波方式）
    ihdr.push_back(0);  // interlace（隔行）
    push_chunk("IHDR", ihdr);

    // IDAT：zlib 头 + stored deflate 块 + 数据
    std::vector<uint8_t> idat;
    idat.push_back(0x78); // CMF
    idat.push_back(0x01); // FLG
    // stored deflate 块：BFINAL=1, BTYPE=00 → 字节 0x01
    idat.push_back(0x01);
    // LEN = 每行 (1 filter + width*3)，共 height 行
    uint32_t raw_len = static_cast<uint32_t>(height) * (1 + width * 3);
    idat.push_back(static_cast<uint8_t>(raw_len & 0xFF));
    idat.push_back(static_cast<uint8_t>((raw_len >> 8) & 0xFF));
    // NLEN = ~LEN
    uint32_t nlen = ~raw_len;
    idat.push_back(static_cast<uint8_t>(nlen & 0xFF));
    idat.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
    // 数据：每行 filter=0 + RGB 像素
    for (size_t i = 0; i < rgb_rows.size(); i += width * 3) {
        idat.push_back(0); // filter 0（无滤波）
        for (int x = 0; x < width * 3; ++x)
            idat.push_back(rgb_rows[i + x]);
    }
    // Adler32（占位，解码器不校验）
    idat.push_back(0); idat.push_back(0); idat.push_back(0); idat.push_back(0);
    push_chunk("IDAT", idat);

    // IEND
    push_chunk("IEND", {});
    return png;
}

QTEST(png_decode_1x1_rgb) {
    // 1x1 红色像素
    std::vector<uint8_t> rows = {255, 0, 0};
    std::vector<uint8_t> png = make_minimal_png(1, 1, rows);

    DecodedImage img = decode_png(png);
    QCHECK(img.valid);
    QCHECK(img.width == 1);
    QCHECK(img.height == 1);
    QCHECK(img.pixels.size() == 4);
    QCHECK(img.pixels[0] == 255); // R
    QCHECK(img.pixels[1] == 0);   // G
    QCHECK(img.pixels[2] == 0);   // B
    QCHECK(img.pixels[3] == 255); // A
}

QTEST(png_decode_2x1_rgb) {
    // 2x1：红 + 蓝
    std::vector<uint8_t> rows = {255, 0, 0, 0, 0, 255};
    std::vector<uint8_t> png = make_minimal_png(2, 1, rows);

    DecodedImage img = decode_png(png);
    QCHECK(img.valid);
    QCHECK(img.width == 2);
    QCHECK(img.height == 1);
    // 第一个像素红
    QCHECK(img.pixels[0] == 255);
    QCHECK(img.pixels[1] == 0);
    QCHECK(img.pixels[2] == 0);
    // 第二个像素蓝
    QCHECK(img.pixels[4] == 0);
    QCHECK(img.pixels[5] == 0);
    QCHECK(img.pixels[6] == 255);
}

QTEST(png_decode_invalid) {
    bool threw = false;
    try {
        decode_png({1, 2, 3, 4, 5});
    } catch (const std::exception &) {
        threw = true;
    }
    QCHECK(threw);
}

QTEST(texture_decoder_dispatch) {
    // 通过 TextureDecoder::decode 按 mime 分派
    std::vector<uint8_t> rows = {0, 255, 0}; // 绿
    std::vector<uint8_t> png = make_minimal_png(1, 1, rows);

    Texture tex;
    tex.data = png;
    tex.mime = "image/png";
    tex.valid = true;

    DecodedImage img = TextureDecoder::decode(tex);
    QCHECK(img.valid);
    QCHECK(img.pixels[1] == 255); // G 通道
}
