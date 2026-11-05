#pragma once
//
// fox/image/png.hpp —— fox 自己的 PNG 解码 + mipmap 链 + 三线性采样
//
// 为什么自写 inflate 而不是引 stb_image
// ------------------------------------
// fox 的立身之本（README 第 0 节）是"删掉 Nuklear 之后仍然能构建"。往里塞
// 另一份第三方实现，等于把"自有"换成"又藏了一份别人的代码"：它的许可证、
// 它的维护状况、它的行为怪癖都会跟着进来。而 PNG 真正需要的那部分 DEFLATE
// 是 RFC1951 的**穷尽子集**（stored / fixed / dynamic 三种块、LZ77 回溯），
// 大约 250 行，而且**可以用已知向量精确验证** —— 压缩后的字节里有 CRC 和
// Adler-32 两道校验，解错了不会静默。
//
// 这一层不碰 GPU、不碰窗口，可以（也应该）被纯逻辑测试驱动：
//   runtime/tests/fox_png_test.cpp —— 已知向量 + 三种块类型覆盖 + 负例
//
// 明确不做（写在这里，而不是让调用者碰运气）：
//   · Adam7 隔行扫描 —— 返回错误而不是解出错图；
//   · 16 位通道按"取高 8 位"降位（这是不可逆的，写进注释而不是假装无损）；
//   · PNG 之外的格式（APNG 动画帧按普通帧处理、ICC 忽略）。
//
#include "core/types.hpp"
#include <string>
#include <vector>

namespace fox
{
    namespace img
    {
        // ── inflate（RFC 1950 zlib 流）────────────────────────────────────

        /// 每种 DEFLATE 块出现了多少次。测试用它来证明"三种块都真的被走过"，
        /// 而不是"向量恰好只覆盖了 dynamic"。
        struct InflateStats
        {
            u32 stored = 0;
            u32 fixed = 0;
            u32 dynamic = 0;
        };

        /// 解 zlib 流（2 字节头 + DEFLATE + 4 字节 Adler-32）。
        ///
        /// 失败返回 false 并（可选）写出原因。**不抛异常**：这是一个 C ABI
        /// 背后的库，异常穿过 C 边界是 UB。所有长度、索引都在边界上检查。
        /// `max_out` 是解压炸弹的闸门：超过就失败，而不是把内存吃光。
        bool inflate_zlib(const u8 *src, size_t n, std::vector<u8> &out,
                          InflateStats *stats = nullptr, std::string *err = nullptr,
                          size_t max_out = (size_t)1 << 28);

        // ── PNG ──────────────────────────────────────────────────────────

        struct Image
        {
            u32 w = 0;
            u32 h = 0;
            std::vector<u8> rgba; ///< w*h*4, 直通（非预乘）alpha，与 Texture 一致
        };

        /// 解码一张 PNG 到 RGBA8。
        ///
        /// 支持：颜色类型 0/2/3/4/6（灰、RGB、调色板、灰+alpha、RGBA），
        /// 位深 1/2/4/8/16（16 位取高位字节；调色板 1/2/4/8——PNG 规范只
        /// 允许这几种组合，其它组合明确报错）。行滤波器 0..4、每块 CRC 校验。
        bool decode_png(const u8 *src, size_t n, Image &out, std::string *err = nullptr);

        // ── mipmap 链 ────────────────────────────────────────────────────

        struct MipChain
        {
            u32 w = 0; ///< 第 0 级尺寸
            u32 h = 0;
            std::vector<std::vector<u8>> levels; ///< levels[0] = 原图，逐级减半，止于 1x1
            u32 count() const { return (u32)levels.size(); }
        };

        /// 生成 mipmap 链（box 平均，止于 1x1）。
        ///
        /// 在**预乘**空间里平均、再解预乘：直通 alpha 的图直接平均颜色，会把
        /// 透明区域的黑色混进边缘（图标一圈灰边，就是这么做出来的）。
        MipChain build_mips(u32 w, u32 h, const u8 *rgba);

        /// 双线性采样某一级。`u,v` 是归一化坐标（0..1），边缘钳位 —— 与
        /// VkBackend 的采样器（CLAMP_TO_EDGE + LINEAR）同一条规则。
        void sample_bilinear(const MipChain &m, u32 level, f32 u, f32 v, u8 out[4]);

        /// 三线性采样：两级双线性结果按 lod 小数部分插值。
        ///
        /// lod 由 `mip_lod()` 从"一个像素覆盖多少 texel"算出来；lod 超出
        /// 链尾时钳到最后一级（1x1），不会越界。
        void sample_trilinear(const MipChain &m, f32 u, f32 v, f32 lod, u8 out[4]);

        /// texels_per_pixel → lod。缩放倍率 1:1 时为 0，放大时为 0（放大不
        /// 需要 mip），缩小 2 倍为 1。
        f32 mip_lod(f32 texels_per_pixel);

        // ── zlib（压缩方向）─────────────────────────────────────────────
        //
        // 只用于测试与"自检"：把一段字节压成 zlib 流，再让 inflate 解回来，
        // 是 inflate 唯一不依赖外部工具的端到端验证。故意只实现 stored 块
        // （最快、无 Huffman 表、格式最简单）—— 它的价值是当**参照物**，
        // 不是当压缩器。
        std::vector<u8> zlib_store(const u8 *src, size_t n);
    } // namespace img
} // namespace fox
