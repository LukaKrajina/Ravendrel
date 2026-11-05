#pragma once
// ============================================================
// Pa.hpp —— 像素级加速器（Pixel Accelerator）
//
// 用途：给 ciUd/fox 的渲染路径降低 CPU/GPU 占用，且**不改变任何一个像素**。
//
// 为什么"不改变像素"成了硬约束
// ---------------------------
// fox 有两个后端（Vulkan / CPU）。CPU 后端同时是着色器 shaders.frag 的**参考实现**：
// 两者的像素不一致时，唯一能判定"谁在说谎"的手段就是逐位对比。任何"优化"只要
// 改动像素，就等于把这个判定手段拆掉了 —— 省下来的 CPU 时间换来的是再也不能
// 自动判定的两条路径。
//
// 因此这里只有四类**可证明等价**的手段（研究依据见 docs/gfx-acceleration.md）：
//
//   1. DamageTracker   —— 内容哈希未变的 tile 不重画（部分重绘）
//   2. dirty_tiles_for —— 每实例只遍历真正可能改变的 tile
//   3. core_span       —— 圆角矩形"内部"区域 alpha 恒为 1，跳过 SDF 求值与混合
//   4. drop_occluded   —— 被后继不透明实例整块盖住的实例，直接从列表里删掉
//
// 前三条只对 CPU 后端生效（它持有跨帧缓冲，部分重绘天然正确；交换链多帧在飞时
// 不成立）。第 4 条在实例构建阶段生效，**两个后端同时受益**。
//
// 与 fox 的关系
// -------------
// 本文件是**项目提供给 fox 的**加速器，不是 fox 的一部分：fox 通过
// `CpuBackend::set_accel(false)` 关掉它（等价于旧的 FOX_NO_PA，但不需要重编译），
// 关掉后走的就是逐像素参考路径 —— 同样的输入、同样的输出，只是慢一些。
// 这条"关掉即参考实现"的性质是**测试的基础**：pa_parity_test 就是靠它把两份
// 结果逐字节比出来的。
// 为了这个性质，本文件不 include 任何 fox 头，也不认识 fox 的类型 —— 它只处理
// 浮点矩形、不透明度、和 64 位哈希。
//
// ============================================================

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pa
{
    using u8 = std::uint8_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i32 = std::int32_t;
    using f32 = float;

    // ── 内容哈希 ────────────────────────────────────────────────────────
    //
    // 要求：同样字节序列必须永远给同样结果（跨帧、跨进程），否则 damage 判定
    // 会假阳性（多画，安全）或假阴性（漏画，错误）。所以这里是一个固定的
    // 64 位混合函数，不用 std::hash（其实现可变，且对 float 的语义没有保证）。

    /// splitmix64 的终混合。雪崩性质足够好，且只有 4 条乘法。
    inline u64 mix64(u64 x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    /// FNV-1a 64 位。逐字节读取，因此浮点的位模式（含 -0.0 / NaN）都被算进去：
    /// 像素会变的输入必须改变哈希，宁可多一个"其实没变"的位（多画一帧）。
    inline u64 hash_bytes(const void *p, size_t n, u64 seed = 0xCBF29CE484222325ull)
    {
        const u8 *b = (const u8 *)p;
        u64 h = seed;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= (u64)b[i];
            h *= 0x100000001B3ull;
        }
        return h;
    }

    /// 8 字节步进的哈希：结果同样是确定的，但每 8 字节只做 2 次乘法，
    /// 而不是 8 次。
    ///
    /// 为什么需要它的第二个版本：实例是 96 字节的 POD，每帧要哈希上千个
    /// （登记一遍、比较一遍）。按字节做 FNV 的话，这条路径的开销会直接吃
    /// 掉 damage 省下来的时间 —— 基准里量到过 0.99× 的"加速比"，也就是
    /// 白忙。改成按字哈希后同一场景才有净收益。
    inline u64 hash_words(const void *p, size_t n, u64 seed = 0xCBF29CE484222325ull)
    {
        const u8 *b = (const u8 *)p;
        u64 h = seed ^ (n * 0x9E3779B97F4A7C15ull);
        size_t i = 0;
        for (; i + 8 <= n; i += 8)
        {
            u64 w;
            std::memcpy(&w, b + i, 8);
            h = mix64(h ^ w);
        }
        if (i < n)
        {
            u64 w = 0;
            std::memcpy(&w, b + i, n - i);
            h = mix64(h ^ w);
        }
        return h;
    }

    template <typename T>
    inline u64 hash_pod(const T &v, u64 seed = 0xCBF29CE484222325ull)
    {
        return hash_words(&v, sizeof(T), seed);
    }

    // ── tile 几何 ───────────────────────────────────────────────────────

    struct TileRect
    {
        i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; // [x0,x1) × [y0,y1)，像素单位
        bool empty() const { return x1 <= x0 || y1 <= y0; }
        i32 width() const { return x1 - x0; }
        i32 height() const { return y1 - y0; }
    };

    // ── 1. acquire 跟踪（damage tracking）────────────────────────────────

    /// 帧间 tile 级差异跟踪。
    ///
    /// 每帧把"会写入这块像素的东西"的哈希喂进来；下一帧同一 tile 的哈希不变
    /// 就说明这块像素不会变，可以整块不画。判定是**保守**的：任何不确定
    /// （首帧、尺寸变化、纹理代际变化）都会导致全屏重画。
    class DamageTracker
    {
    public:
        static constexpr i32 kTile = 16;

        void reset(i32 w, i32 h)
        {
            width_ = w > 0 ? w : 0;
            height_ = h > 0 ? h : 0;
            tiles_x_ = (width_ + kTile - 1) / kTile;
            tiles_y_ = (height_ + kTile - 1) / kTile;
            const size_t n = (size_t)tiles_x_ * (size_t)tiles_y_;
            cur_.assign(n, 0);
            prev_.assign(n, 0);
            have_prev_ = false;
            salt_ = 0;
            frame_salt_ = 0;
            dirty_count_ = tiles_x_ * tiles_y_;
            skipped_area_ = 0;
        }

        bool enabled() const { return enabled_; }
        void set_enabled(bool on)
        {
            if (enabled_ == on) return;
            enabled_ = on;
            invalidate_all(); // 开/关都是一次语义切换，从干净状态重新开始
        }

        i32 width() const { return width_; }
        i32 height() const { return height_; }
        i32 tiles_x() const { return tiles_x_; }
        i32 tiles_y() const { return tiles_y_; }
        u32 tile_count() const { return (u32)((size_t)tiles_x_ * (size_t)tiles_y_); }

        /// 开一帧。`salt` 是"影响全部像素的东西"（主题背景色、纹理/字形图集的
        /// 代际号……）。salt 变了 ⇒ 全屏重画：纹理重传后即使显示列表一模一样，
        /// 像素也会变。
        void begin_frame(u64 salt)
        {
            if (width_ <= 0 || height_ <= 0) return;
            frame_salt_ = salt;
            for (size_t i = 0; i < cur_.size(); ++i)
            {
                cur_[i] = mix64(salt ^ (u64)i);
            }
            // ⚠ 上一帧的 salt 与本帧不同 ⇒ 全屏重画。只需比较最近一次的值，
            //   因为 prev_ 里的哈希只在 salt 相同的前提下才有意义。
            if (!have_prev_ || salt != salt_)
            {
                have_prev_ = false;
            }
        }

        /// 登记一个"会覆盖 [x0,y0,x1,y1) 的内容"。坐标是像素，允许越界
        /// （会按 tile 范围裁剪）。`content` 必须覆盖该内容的**全部**输入：
        /// 几何、颜色、以及任何被采样纹理的代际号。
        void touch(i32 x0, i32 y0, i32 x1, i32 y1, u64 content)
        {
            if (width_ <= 0 || height_ <= 0) return;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > width_) x1 = width_;
            if (y1 > height_) y1 = height_;
            if (x1 <= x0 || y1 <= y0) return;

            const i32 tx0 = x0 / kTile, tx1 = (x1 - 1) / kTile;
            const i32 ty0 = y0 / kTile, ty1 = (y1 - 1) / kTile;
            const u64 h = mix64(content);
            for (i32 ty = ty0; ty <= ty1; ++ty)
            {
                u64 *row = &cur_[(size_t)ty * (size_t)tiles_x_];
                for (i32 tx = tx0; tx <= tx1; ++tx)
                {
                    // 与 tile 索引混一次：两个内容相同的实例交换位置时，
                    // 纯 XOR 会互相抵消（h^h==0），tile 就被误判成"没变"。
                    row[tx] ^= mix64(h ^ ((u64)tx << 32) ^ (u64)ty);
                }
            }
        }

        /// 结束本帧：结算脏 tile 数量与跳过面积。
        void end_frame()
        {
            dirty_count_ = 0;
            skipped_area_ = 0;
            if (width_ <= 0 || height_ <= 0) return;
            const size_t n = cur_.size();
            if (!have_prev_)
            {
                dirty_count_ = (u32)n;
                prev_ = cur_;
                have_prev_ = true;
                salt_ = frame_salt_;
                return;
            }
            for (size_t i = 0; i < n; ++i)
            {
                if (prev_[i] != cur_[i])
                {
                    ++dirty_count_;
                }
                else
                {
                    // 干净 tile 的像素数（最后一行/列可能不足 16）
                    const i32 tx = (i32)(i % (size_t)tiles_x_);
                    const i32 ty = (i32)(i / (size_t)tiles_x_);
                    const i32 w = std::min(kTile, width_ - tx * kTile);
                    const i32 h = std::min(kTile, height_ - ty * kTile);
                    skipped_area_ += (u64)(w > 0 ? w : 0) * (u64)(h > 0 ? h : 0);
                }
            }
            prev_ = cur_;
            salt_ = frame_salt_;
        }

        /// 强制全屏重画（例如窗口缩放、后端切换）。
        void invalidate_all()
        {
            have_prev_ = false;
            dirty_count_ = tiles_x_ > 0 ? (u32)((size_t)tiles_x_ * (size_t)tiles_y_) : 0;
        }

        bool all_dirty() const { return !have_prev_; }
        bool any_dirty() const { return all_dirty() || dirty_count_ > 0; }

        /// 遍历本帧的脏区域。首帧/失效时给出整个矩形（一次）。
        /// 回调签名：fn(TileRect)。
        template <typename Fn>
        void for_each_dirty(Fn &&fn) const
        {
            if (width_ <= 0 || height_ <= 0) return;
            if (!have_prev_)
            {
                fn(TileRect{0, 0, width_, height_});
                return;
            }
            for (i32 ty = 0; ty < tiles_y_; ++ty)
            {
                const size_t row = (size_t)ty * (size_t)tiles_x_;
                for (i32 tx = 0; tx < tiles_x_; ++tx)
                {
                    if (prev_[row + (size_t)tx] == cur_[row + (size_t)tx]) continue;
                    fn(TileRect{tx * kTile, ty * kTile,
                                std::min((tx + 1) * kTile, width_),
                                std::min((ty + 1) * kTile, height_)});
                }
            }
        }

        bool dirty(i32 tx, i32 ty) const
        {
            if (tx < 0 || ty < 0 || tx >= tiles_x_ || ty >= tiles_y_) return false;
            if (!have_prev_) return true;
            const size_t i = (size_t)ty * (size_t)tiles_x_ + (size_t)tx;
            return prev_[i] != cur_[i];
        }

        /// 收集与 [x0,y0,x1,y1) 相交的**脏** tile 矩形，最多 `cap` 个。
        /// 返回 0 表示该矩形整体落在干净 tile 上（可以完全跳过）；
        /// 返回值 == cap 表示"给的容量不够"，调用方必须退化为整个矩形。
        i32 dirty_tiles_for(i32 x0, i32 y0, i32 x1, i32 y1, TileRect *out, i32 cap) const
        {
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > width_) x1 = width_;
            if (y1 > height_) y1 = height_;
            if (x1 <= x0 || y1 <= y0) return 0;
            if (!have_prev_)
            {
                if (cap < 1) return cap;
                out[0] = TileRect{x0, y0, x1, y1};
                return 1;
            }

            const i32 tx0 = x0 / kTile, tx1 = (x1 - 1) / kTile;
            const i32 ty0 = y0 / kTile, ty1 = (y1 - 1) / kTile;
            i32 n = 0;
            for (i32 ty = ty0; ty <= ty1; ++ty)
            {
                const size_t row = (size_t)ty * (size_t)tiles_x_;
                for (i32 tx = tx0; tx <= tx1; ++tx)
                {
                    if (prev_[row + (size_t)tx] == cur_[row + (size_t)tx]) continue;
                    if (n >= cap) return cap;
                    const i32 cx0 = std::max(x0, tx * kTile);
                    const i32 cx1 = std::min(x1, (tx + 1) * kTile);
                    // ⚠ y0 也必须夹到请求矩形（和 x 对称）。只夹 y1 时，实例
                    //   顶边上方的半格会被当成"要遍历的矩形"交给光栅器：有
                    //   覆盖率测试的图元（圆角矩形/椭圆/三角形/文字/玻璃）
                    //   在形状外写不进去，看起来"没事"；**图片没有覆盖率测试**
                    //   ——它在矩形内逐像素采样纹理 —— 于是图片会向上渗出最多
                    //   15 行。图标是透明边缘，所以旧场景看不出来；不透明的
                    //   纹理（fox_png/pa_parity 场景 7）会当场露馅。
                    const i32 cy0 = std::max(y0, ty * kTile);
                    const i32 cy1 = std::min(y1, (ty + 1) * kTile);
                    out[n++] = TileRect{cx0, cy0, cx1, cy1};
                }
            }
            return n;
        }

        u32 dirty_count() const { return have_prev_ ? dirty_count_ : tile_count(); }
        /// 本帧**没有**被重画的像素数（damage 省下的量，用于基准测试）。
        u64 skipped_area() const { return skipped_area_; }
        bool has_history() const { return have_prev_; }

    private:
        std::vector<u64> cur_, prev_;
        i32 width_ = 0, height_ = 0;
        i32 tiles_x_ = 0, tiles_y_ = 0;
        bool have_prev_ = false;
        bool enabled_ = true;
        u64 salt_ = 0, frame_salt_ = 0;
        u32 dirty_count_ = 0;
        u64 skipped_area_ = 0;
    };

    // ── 3. 圆角矩形的"核心跨度" ──────────────────────────────────────────
    //
    // 对 iq 的 sdRoundBox（cpu_backend.cpp / shaders.frag 里的同一份数学）：
    //
    //     qx = |px| - bx + r ,  qy = |py| - by + r
    //     inside:  d = max(qx, qy)        （< 0）
    //
    // 于是"alpha 必然为 1"的区域（smoothstep(-0.5, 0.5, d) == 0 ⇔ d <= -0.5）
    // 有一个纯代数刻画：
    //
    //     |px| <= bx - r - 0.5  且  |py| <= by - r - 0.5
    //
    // 这是一个**轴对齐矩形**，所以每条扫描线的核心区就是一个区间，可以一行算出来。
    // 它不是近似：它就是逐像素路径在该区域上的取值条件，所以结果逐位相同。
    //
    // 注意半径是"四个角都等于 r"时的结论（参考实现里的 uniform 分支用的就是 r0）。

    struct Span
    {
        i32 lo = 0, hi = 0; // [lo, hi)
        bool empty() const { return hi <= lo; }
        i32 width() const { return hi - lo; }
    };

    /// 某一行（像素 y=py）在核心矩形内的跨度。
    /// 全部按"像素中心 px+0.5"计算，与参考路径一致。
    ///
    /// ⚠ 推导（这里错过一次，所以写全）：
    ///   参考实现的 SDF 是规范式
    ///       q = |p| - b + r
    ///       d = min(max(q.x,q.y), 0) + length(max(q,0)) - **r**
    ///   `- r` 是**在 q 之外**减的。所以 d <= -0.5 的条件是
    ///       max(q.x,q.y) <= r - 0.5
    ///   把 q = |p| - b + r 代回去，r 被**消掉**：
    ///       |p.x| <= bx - 0.5  且  |p.y| <= by - 0.5
    ///   也就是说"必然 alpha == 1"的区域是**矩形本身**（与圆角半径无关），
    ///   不是"内缩 r 的矩形"。曾经按后者写，结果把核心区算小了 —— 这不是
    ///   保守的方向，因为核心区边界像素的 alpha 在浮点下可能略小于 1，
    ///   直接覆盖写就和参考路径差 1 个色阶（实测就是这样被抓到的）。
    ///
    /// 现在两侧各留 1 像素余量：多出来的像素走逐像素参考路径，代价可忽略。
    inline Span core_span_row(f32 cx, f32 half_w, f32 half_h, f32 r, i32 py, f32 cy)
    {
        Span s;
        (void)r; // 半径在上面的推导里被消掉了；保留参数是为了调用点可读
        const f32 mw = half_w - 1.0f;
        const f32 mh = half_h - 1.0f;
        if (mw <= 0.0f) return s; // 窄于 3 像素的矩形没有安全核心区

        const f32 dy = ((f32)py + 0.5f) - cy;
        if (dy >= mh || dy <= -mh) return s; // 这行不在核心矩形内（留 1 px 余量）

        // |px + 0.5 - cx| <= half_w - 1
        s.lo = (i32)std::ceil(cx - half_w + 1.0f);
        s.hi = (i32)std::floor(cx + half_w - 2.0f) + 1;
        return s;
    }

    /// 核心区写像素：alpha 恒为 1，所以是**直接覆盖**，与参考路径的
    /// `blend(px,py,r,g,b,1.0f)` 逐位相同（blend 在 a==1 时 inv==0，
    /// 结果与目标像素无关）。用 memset 语义填 4 字节像素。
    inline void fill_row(u8 *row, i32 lo, i32 hi, u8 r, u8 g, u8 b, u8 a = 255)
    {
        if (hi <= lo) return;
        const u32 px = ((u32)r) | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
        u8 *p = row + (size_t)lo * 4;
        const size_t n = (size_t)(hi - lo);
        // 手写循环而不是 memset：像素是 4 字节，且这样编译器会向量化。
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            std::memcpy(p, &px, 4);
        }
    }

    /// 核心区写像素（颜色逐像素变化，例如水平渐变）。`fn(px, out[3])` 写入
    /// 0..1 的 RGB，量纲与参考路径一致，取整规则也一致（+0.5 后截断）。
    template <typename Fn>
    inline void fill_row_cb(u8 *row, i32 lo, i32 hi, Fn &&fn)
    {
        for (i32 x = lo; x < hi; ++x)
        {
            f32 c[3];
            fn(x, c);
            u8 *p = row + (size_t)x * 4;
            p[0] = (u8)(std::fmin(std::fmax(c[0], 0.0f), 1.0f) * 255.0f + 0.5f);
            p[1] = (u8)(std::fmin(std::fmax(c[1], 0.0f), 1.0f) * 255.0f + 0.5f);
            p[2] = (u8)(std::fmin(std::fmax(c[2], 0.0f), 1.0f) * 255.0f + 0.5f);
            p[3] = 255;
        }
    }

    /// 核心区混合（color[3] < 1 的矩形）。与参考路径的 blend() 逐位相同，
    /// 只是**省掉了 SDF 求值与 smoothstep**：核心区里覆盖率恒为 1，
    /// 混合系数变成常量。
    inline void blend_row(u8 *row, i32 lo, i32 hi, f32 r, f32 g, f32 b, f32 a)
    {
        if (hi <= lo || a <= 0.0f) return;
        if (a > 1.0f) a = 1.0f;
        const f32 inv = 1.0f - a;
        for (i32 x = lo; x < hi; ++x)
        {
            u8 *p = row + (size_t)x * 4;
            p[0] = (u8)(std::fmin(std::fmax(r * a + p[0] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
            p[1] = (u8)(std::fmin(std::fmax(g * a + p[1] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
            p[2] = (u8)(std::fmin(std::fmax(b * a + p[2] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
            p[3] = (u8)(std::fmin(a + p[3] * inv, 1.0f) * 255.0f + 0.5f);
        }
    }

    /// 逐像素颜色的核心区混合（水平渐变 + 半透明）。回调契约同 fill_row_cb。
    template <typename Fn>
    inline void blend_row_cb(u8 *row, i32 lo, i32 hi, f32 a, Fn &&fn)
    {
        if (hi <= lo || a <= 0.0f) return;
        if (a > 1.0f) a = 1.0f;
        const f32 inv = 1.0f - a;
        for (i32 x = lo; x < hi; ++x)
        {
            f32 c[3];
            fn(x, c);
            u8 *p = row + (size_t)x * 4;
            p[0] = (u8)(std::fmin(std::fmax(c[0] * a + p[0] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
            p[1] = (u8)(std::fmin(std::fmax(c[1] * a + p[1] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
            p[2] = (u8)(std::fmin(std::fmax(c[2] * a + p[2] * (inv / 255.0f), 0.0f), 1.0f) * 255.0f + 0.5f);
            p[3] = (u8)(std::fmin(a + p[3] * inv, 1.0f) * 255.0f + 0.5f);
        }
    }

    // ── 4. 遮挡剔除 ─────────────────────────────────────────────────────
    //
    // 画家算法下：若实例 j 在 i **之后**绘制，且 j 的"必然不透明覆盖区"
    // （core：见上）完整包含 i 的整个 quad，则 i 的每一个像素都会被 j 覆盖。
    // 那么 i 可以直接从显示列表里删掉 —— 不是近似，是被覆盖。
    //
    // 判定条件里最容易被忽略的是"不透明"：玻璃（PT_GLASS）采样的是背景，
    // 删掉它下面的东西会改变它自己采样到的内容；阴影/边框/文字/图片的覆盖率
    // 一般不足 1。所以 occluder 必须由调用方明确给出 core 矩形，core 为空
    // 就表示"这个实例不能当遮挡物"。

    struct Item
    {
        // quad 边界（像素，半开区间）
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        // 必然不透明覆盖区（像素）。空 ⇒ 不能遮挡。
        f32 cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    };

    inline bool item_has_core(const Item &it)
    {
        return it.cx1 > it.cx0 && it.cy1 > it.cy0;
    }

    /// 标记哪些实例被后继不透明实例完整盖住。`keep` 由调用方持有，长度 n；
    /// 返回被剔除的数量。
    ///
    /// 复杂度：O(n · K)，K 是"跨实例仍然有效的遮挡物"上限。遮挡物是按面积
    /// 保留的（大的先留），所以 K 很小也不会漏掉真正的整屏遮挡物 —— 那种
    /// 遮挡物一定是最大的一批。
    inline u32 drop_occluded(const Item *items, size_t n, u8 *keep, u32 max_occluders = 64)
    {
        if (!items || !keep || n == 0) return 0;
        for (size_t i = 0; i < n; ++i) keep[i] = 1;

        // 从后往前扫：维持一个"已经见过的、可能盖住当前实例的遮挡物"集合。
        struct Occ
        {
            f32 x0, y0, x1, y1, area;
        };
        std::vector<Occ> occ;
        occ.reserve(max_occluders);
        u32 dropped = 0;

        for (size_t k = n; k-- > 0;)
        {
            const Item &it = items[k];
            for (const Occ &o : occ)
            {
                if (it.x0 >= o.x0 && it.y0 >= o.y0 && it.x1 <= o.x1 && it.y1 <= o.y1)
                {
                    keep[k] = 0;
                    ++dropped;
                    break;
                }
            }

            if (item_has_core(it))
            {
                const f32 area = (it.cx1 - it.cx0) * (it.cy1 - it.cy0);
                if (occ.size() < max_occluders)
                {
                    occ.push_back(Occ{it.cx0, it.cy0, it.cx1, it.cy1, area});
                }
                else
                {
                    // 满了：踢掉最小的那个。整屏遮挡物面积最大，永远不会被踢掉。
                    size_t smallest = 0;
                    for (size_t j = 1; j < occ.size(); ++j)
                        if (occ[j].area < occ[smallest].area) smallest = j;
                    if (area > occ[smallest].area)
                        occ[smallest] = Occ{it.cx0, it.cy0, it.cx1, it.cy1, area};
                }
            }
        }
        return dropped;
    }

    // ── 自适应着色率图（GPU 提示，不是 CPU 路径的一部分）────────────────
    //
    // 参考 NVIDIA VRS / NAS 与 DSR：把屏幕切成 tile，每个 tile 单独指定着色率。
    // 生成端**只用已经知道的信息**（实例类型 + 几何），不做任何图像分析 ——
    // NVIDIA 的官方提醒是：生成 rate map 的代价必须小于省下的代价。
    //
    // 安全性由构造保证：一个 tile 只有在
    //   · 没有任何实例盖到它（纯背景，常量色），或
    //   · 被某个实例的"核心区"完整包含（内部无边缘、无渐变、无文字）
    // 时才是 2×2；其余一律 1×1。任何文字/玻璃/阴影/图片/边缘像素所在的 tile
    // 都是 1×1 —— 降采样率只会让它们变糊，不会让它们变错。
    enum class ShadeRate : u8
    {
        R1x1 = 0, // VK_FRAGMENT_SHADING_RATE_1_INVOCATION_PER_PIXEL_NV
        R2x2 = 3, // VK_FRAGMENT_SHADING_RATE_1_INVOCATION_PER_2X2_PIXELS_NV
        R4x4 = 6, // VK_FRAGMENT_SHADING_RATE_1_INVOCATION_PER_4X4_PIXELS_NV
    };

    struct ShadeRateMap
    {
        i32 tiles_x = 0, tiles_y = 0;
        i32 tile = 16;
        std::vector<u8> rate; // 每 tile 一个 ShadeRate

        bool empty() const { return rate.empty(); }
        u8 at(i32 tx, i32 ty) const
        {
            if (tx < 0 || ty < 0 || tx >= tiles_x || ty >= tiles_y) return (u8)ShadeRate::R1x1;
            return rate[(size_t)ty * (size_t)tiles_x + (size_t)tx];
        }
    };

    /// `flat[i]` 非空表示该实例在 [cx0,cy0,cx1,cy1) 内是常量不透明覆盖。
    struct FlatItem
    {
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;          // 实例 quad（含边缘）
        f32 cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;      // 常量区（可为空）
        bool smooth = false;                          // 文字/玻璃/阴影/图片/渐变
    };

    inline ShadeRateMap build_shade_rate_map(const FlatItem *items, size_t n, i32 w, i32 h,
                                            i32 tile = 16)
    {
        ShadeRateMap m;
        if (w <= 0 || h <= 0) return m;
        m.tile = tile;
        m.tiles_x = (w + tile - 1) / tile;
        m.tiles_y = (h + tile - 1) / tile;
        m.rate.assign((size_t)m.tiles_x * (size_t)m.tiles_y, (u8)ShadeRate::R1x1);

        // 背景默认是"平"的；下面凡是盖到 tile 的东西都会把它降回 1×1。
        std::vector<u8> flat((size_t)m.tiles_x * (size_t)m.tiles_y, 1);

        const i32 TX = m.tiles_x, TY = m.tiles_y;
        auto clamp_t = [&](f32 v, i32 hi) {
            i32 i = (i32)v;
            if (i < 0) i = 0;
            if (i >= hi) i = hi - 1;
            return i;
        };

        for (size_t i = 0; i < n; ++i)
        {
            const FlatItem &it = items[i];
            const i32 tx0 = clamp_t(it.x0 / tile, TX), tx1 = clamp_t((it.x1 - 1.0f) / tile, TX);
            const i32 ty0 = clamp_t(it.y0 / tile, TY), ty1 = clamp_t((it.y1 - 1.0f) / tile, TY);
            for (i32 ty = ty0; ty <= ty1; ++ty)
            {
                for (i32 tx = tx0; tx <= tx1; ++tx)
                {
                    const size_t k = (size_t)ty * (size_t)TX + (size_t)tx;
                    if (it.smooth)
                    {
                        flat[k] = 0;
                        continue;
                    }
                    // 非 smooth 也只有在"整个 tile 落在常量区"时才允许降采样率。
                    const f32 px0 = (f32)(tx * tile), py0 = (f32)(ty * tile);
                    const f32 px1 = (f32)std::min((tx + 1) * tile, w);
                    const f32 py1 = (f32)std::min((ty + 1) * tile, h);
                    const bool inside = px0 >= it.cx0 - 0.5f && py0 >= it.cy0 - 0.5f &&
                                        px1 <= it.cx1 + 0.5f && py1 <= it.cy1 + 0.5f;
                    if (!inside) flat[k] = 0;
                }
            }
        }

        for (size_t k = 0; k < m.rate.size(); ++k)
        {
            m.rate[k] = flat[k] ? (u8)ShadeRate::R2x2 : (u8)ShadeRate::R1x1;
        }
        return m;
    }

    // ── 统计 ────────────────────────────────────────────────────────────
    //
    // "优化生效了吗"必须是一个可以读出来的数。基准测试读它，测试也读它
    // （例如"静态帧的第二帧必须跳过 > 90% 的像素"，否则 damage 没起作用，
    //  只是白白加了一层哈希）。
    struct Stats
    {
        u64 frames = 0;
        u64 instances_drawn = 0;    // 真正进入像素循环的实例
        u64 instances_skipped = 0;  // 落在干净 tile 上被整个跳过的实例
        u64 core_pixels = 0;        // 走核心区快速路径的像素
        u64 shaded_pixels = 0;      // 走逐像素 SDF 的像素
        u64 pixels_skipped = 0;     // damage 省下的像素
        u64 hidden_instances = 0;   // 遮挡剔除掉的实例

        // 分段时间（纳秒，只统计加速器自己的三段：登记内容哈希 / 清屏 / 光栅化）。
        // 没有这三个数，"全屏动画为什么没有加速"就只能靠猜 —— 分开之后
        // 一眼能看出是哈希在花钱，还是光栅化本来就没被省下。
        u64 ns_track = 0;
        u64 ns_clear = 0;
        u64 ns_draw = 0;

        void reset() { *this = Stats{}; }
    };
}