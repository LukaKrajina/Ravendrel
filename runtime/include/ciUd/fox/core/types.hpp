#pragma once
// Fox core/types.hpp - 基础数学类型、几何、颜色、ID
//
// Fox 最底层：只有基础类型和内联运算，不依赖任何其他 Fox 模块。
// 其他层都建立在这一层之上。

#include <cstdint>
#include <cmath>

namespace fox
{
    // ── 基础类型 ────────────────────────────────────────────────────

    using u8 = uint8_t;
    using u16 = uint16_t;
    using u32 = uint32_t;
    using u64 = uint64_t;
    using i32 = int32_t;
    using i64 = int64_t;
    using f32 = float;
    using f64 = double;

    inline constexpr f32 PI = 3.14159265358979323846f;

    inline f32 fx_min(f32 a, f32 b) { return a < b ? a : b; }
    inline f32 fx_max(f32 a, f32 b) { return a > b ? a : b; }
    inline f32 fx_clamp(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
    inline f32 fx_abs(f32 v) { return v < 0.0f ? -v : v; }
    inline f32 fx_floor(f32 v) { return std::floor(v); }
    inline f32 fx_ceil(f32 v) { return std::ceil(v); }
    inline f32 fx_round(f32 v) { return std::round(v); }
    inline f32 fx_sqrt(f32 v) { return std::sqrt(v); }
    inline f32 fx_sin(f32 a) { return std::sin(a); }
    inline f32 fx_cos(f32 a) { return std::cos(a); }
    inline f32 fx_atan2(f32 y, f32 x) { return std::atan2(y, x); }

    /// 线性插值，t 在 [0,1] 内
    inline f32 fx_lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

    /// 带截断的线性插值，用于颜色/浮点数
    inline f32 fx_mix(f32 a, f32 b, f32 t) { return fx_lerp(a, b, fx_clamp(t, 0.0f, 1.0f)); }

    /// 两边缘间的平滑 Hermite 插值。
    inline f32 fx_smoothstep(f32 edge0, f32 edge1, f32 x)
    {
        if (edge0 == edge1) return x < edge0 ? 0.0f : 1.0f;
        f32 t = fx_clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // ── Vec2 ───────────────────────────────────────────────────────────

    struct Vec2
    {
        f32 x = 0.0f;
        f32 y = 0.0f;

        Vec2() = default;
        Vec2(f32 xx, f32 yy) : x(xx), y(yy) {}

        Vec2 operator+(const Vec2 &o) const { return {x + o.x, y + o.y}; }
        Vec2 operator-(const Vec2 &o) const { return {x - o.x, y - o.y}; }
        Vec2 operator*(f32 s) const { return {x * s, y * s}; }
        Vec2 operator*(const Vec2 &o) const { return {x * o.x, y * o.y}; }
        Vec2 &operator+=(const Vec2 &o) { x += o.x; y += o.y; return *this; }
        Vec2 &operator-=(const Vec2 &o) { x -= o.x; y -= o.y; return *this; }
        bool operator==(const Vec2 &o) const { return x == o.x && y == o.y; }
        bool operator!=(const Vec2 &o) const { return !(*this == o); }
    };

    inline Vec2 vec2(f32 x, f32 y) { return {x, y}; }

    // ── Rect ───────────────────────────────────────────────────────────
    //
    // 注意：x/y 是左上角，w/h 是尺寸（rvdl 语言的 cgfx_* 图元也用同样的约定）。

    struct Rect
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 w = 0.0f;
        f32 h = 0.0f;

        Rect() = default;
        Rect(f32 xx, f32 yy, f32 ww, f32 hh) : x(xx), y(yy), w(ww), h(hh) {}

        f32 left() const { return x; }
        f32 top() const { return y; }
        f32 right() const { return x + w; }
        f32 bottom() const { return y + h; }
        f32 cx() const { return x + w * 0.5f; }
        f32 cy() const { return y + h * 0.5f; }
        Vec2 center() const { return {cx(), cy()}; }

        bool empty() const { return w <= 0.0f || h <= 0.0f; }
        bool contains(const Vec2 &p) const
        {
            return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
        }

        Rect translated(const Vec2 &d) const { return {x + d.x, y + d.y, w, h}; }

        Rect inset(f32 dx, f32 dy) const { return {x + dx, y + dy, w - dx * 2.0f, h - dy * 2.0f}; }
        Rect inset(f32 all) const { return inset(all, all); }

        Rect expanded(f32 dx, f32 dy) const { return {x - dx, y - dy, w + dx * 2.0f, h + dy * 2.0f}; }

        bool intersects(const Rect &o) const
        {
            return !(o.x >= right() || o.right() <= x || o.y >= bottom() || o.bottom() <= y);
        }

        /// 交集矩形。不重叠时为空（w 或 h <= 0）。
        Rect intersected(const Rect &o) const
        {
            const f32 l = fx_max(x, o.x);
            const f32 t = fx_max(y, o.y);
            const f32 r = fx_min(right(), o.right());
            const f32 b = fx_min(bottom(), o.bottom());
            if (r <= l || b <= t) return {l, t, 0.0f, 0.0f};
            return {l, t, r - l, b - t};
        }

        static Rect united(const Rect &a, const Rect &b)
        {
            if (a.empty()) return b;
            if (b.empty()) return a;
            f32 l = fx_min(a.x, b.x);
            f32 t = fx_min(a.y, b.y);
            f32 r = fx_max(a.right(), b.right());
            f32 bo = fx_max(a.bottom(), b.bottom());
            return {l, t, r - l, bo - t};
        }
    };

    inline Rect rect(f32 x, f32 y, f32 w, f32 h) { return {x, y, w, h}; }
    inline Rect rect_wh(f32 w, f32 h) { return {0.0f, 0.0f, w, h}; }

    // ── 颜色（sRGB 字节 RGBA）────────────────────────────────────────

    struct Color
    {
        u8 r = 0, g = 0, b = 0, a = 255;

        Color() = default;
        Color(u8 rr, u8 gg, u8 bb, u8 aa = 255) : r(rr), g(gg), b(bb), a(aa) {}

        /// 0xRRGGBB 打包（与 rvdl 的 cgfx_* 颜色参数兼容）
        static Color rgb(u32 v, u8 alpha = 255)
        {
            return {(u8)((v >> 16) & 0xFF), (u8)((v >> 8) & 0xFF), (u8)(v & 0xFF), alpha};
        }

        u32 packed() const { return ((u32)r << 16) | ((u32)g << 8) | (u32)b; }

        /// 在 sRGB 空间线性混合（开销小；与 macOS 大多数扁平 UI 填充的做法一致）。
        static Color mix(const Color &a, const Color &b, f32 t)
        {
            t = fx_clamp(t, 0.0f, 1.0f);
            auto lerp8 = [&](u8 x, u8 y) { return (u8)(x + (f32)(y - x) * t + 0.5f); };
            return {lerp8(a.r, b.r), lerp8(a.g, b.g), lerp8(a.b, b.b), lerp8(a.a, b.a)};
        }

        Color with_alpha(u8 na) const { return {r, g, b, na}; }

        f32 luminance() const
        {
            // Rec. 709 亮度公式，用于自动对比度的文本选色。
            return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
        }
    };

    inline Color rgba(u8 r, u8 g, u8 b, u8 a = 255) { return {r, g, b, a}; }

    /// 从 0xRRGGBB 字面量构造颜色（主题里统一用这种写法）。
    inline Color rgb(u32 v, u8 a = 255) { return Color::rgb(v, a); }

    // ── ID ─────────────────────────────────────────────────────────────
    //
    // 立即模式下每个控件仍需稳定标识，让状态（悬停、焦点、动画）能跨帧
    // 保留。用（显式 id，树中位置）的 32 位哈希。可能冲突，但罕见且无害
    // （最坏情况：两个控件短暂共享悬停状态）。

    using Id = u32;

    inline Id id_hash(Id salt, Id value)
    {
        // 类 FNV-1a 混合。
        u32 h = salt ^ value;
        h ^= h >> 16;
        h *= 0x7feb352dU;
        h ^= h >> 15;
        h *= 0x846ca68bU;
        h ^= h >> 16;
        return h;
    }

    /// 把矩形哈希成 id，用于没有标签可作 key 的控件（文本框、滑块——
    /// 纯靠布局定位的控件）。
    ///
    /// 刻意对齐到整像素：布局重算出小数偏移时，id 若跟着变，控件就会
    /// "变成另一个控件"，会直接丢掉悬停动画、滚动位置和光标。
    inline Id id_hash_rect(const Rect &r)
    {
        auto q = [](f32 v) -> i64 { return (i64)(v * 0.5f + (v < 0.0f ? -0.5f : 0.5f)); };
        const i64 x = q(r.x), y = q(r.y), w = q(r.w), h = q(r.h);
        u32 v = (u32)x * 0x9E3779B1u;
        v ^= (u32)y * 0x85EBCA77u;
        v ^= (u32)w * 0xC2B2AE3Du;
        v ^= (u32)h * 0x27D4EB2Fu;
        return id_hash(0x51ED2701u, v);
    }

    // ── 缓动函数（动画用）─────────────────────────────────────────

    namespace ease
    {
        inline f32 linear(f32 t) { return t; }
        inline f32 in_quad(f32 t) { return t * t; }
        inline f32 out_quad(f32 t) { return t * (2.0f - t); }
        inline f32 in_out_quad(f32 t)
        {
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        }
        inline f32 out_cubic(f32 t)
        {
            f32 u = t - 1.0f;
            return u * u * u + 1.0f;
        }
        /// 类似 macOS 的弹簧效果近似（不过冲，快速稳定）。
        inline f32 out_quint(f32 t)
        {
            f32 u = t - 1.0f;
            return u * u * u * u * u + 1.0f;
        }
    } // namespace ease

} // namespace fox