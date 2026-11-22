#pragma once

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


    inline u64 mix64(u64 x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

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


    struct TileRect
    {
        i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool empty() const { return x1 <= x0 || y1 <= y0; }
        i32 width() const { return x1 - x0; }
        i32 height() const { return y1 - y0; }
    };


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
            invalidate_all();
        }

        i32 width() const { return width_; }
        i32 height() const { return height_; }
        i32 tiles_x() const { return tiles_x_; }
        i32 tiles_y() const { return tiles_y_; }
        u32 tile_count() const { return (u32)((size_t)tiles_x_ * (size_t)tiles_y_); }

        void begin_frame(u64 salt)
        {
            if (width_ <= 0 || height_ <= 0) return;
            frame_salt_ = salt;
            for (size_t i = 0; i < cur_.size(); ++i)
            {
                cur_[i] = mix64(salt ^ (u64)i);
            }
            if (!have_prev_ || salt != salt_)
            {
                have_prev_ = false;
            }
        }

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
                    row[tx] ^= mix64(h ^ ((u64)tx << 32) ^ (u64)ty);
                }
            }
        }

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

        void invalidate_all()
        {
            have_prev_ = false;
            dirty_count_ = tiles_x_ > 0 ? (u32)((size_t)tiles_x_ * (size_t)tiles_y_) : 0;
        }

        bool all_dirty() const { return !have_prev_; }
        bool any_dirty() const { return all_dirty() || dirty_count_ > 0; }

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
                    const i32 cy0 = std::max(y0, ty * kTile);
                    const i32 cy1 = std::min(y1, (ty + 1) * kTile);
                    out[n++] = TileRect{cx0, cy0, cx1, cy1};
                }
            }
            return n;
        }

        u32 dirty_count() const { return have_prev_ ? dirty_count_ : tile_count(); }
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


    struct Span
    {
        i32 lo = 0, hi = 0;
        bool empty() const { return hi <= lo; }
        i32 width() const { return hi - lo; }
    };

    inline Span core_span_row(f32 cx, f32 half_w, f32 half_h, f32 r, i32 py, f32 cy)
    {
        Span s;
        (void)r;
        const f32 mw = half_w - 1.0f;
        const f32 mh = half_h - 1.0f;
        if (mw <= 0.0f) return s;

        const f32 dy = ((f32)py + 0.5f) - cy;
        if (dy >= mh || dy <= -mh) return s;

        s.lo = (i32)std::ceil(cx - half_w + 1.0f);
        s.hi = (i32)std::floor(cx + half_w - 2.0f) + 1;
        return s;
    }

    inline void fill_row(u8 *row, i32 lo, i32 hi, u8 r, u8 g, u8 b, u8 a = 255)
    {
        if (hi <= lo) return;
        const u32 px = ((u32)r) | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
        u8 *p = row + (size_t)lo * 4;
        const size_t n = (size_t)(hi - lo);
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            std::memcpy(p, &px, 4);
        }
    }

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


    struct Item
    {
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        f32 cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    };

    inline bool item_has_core(const Item &it)
    {
        return it.cx1 > it.cx0 && it.cy1 > it.cy0;
    }

    inline u32 drop_occluded(const Item *items, size_t n, u8 *keep, u32 max_occluders = 64)
    {
        if (!items || !keep || n == 0) return 0;
        for (size_t i = 0; i < n; ++i) keep[i] = 1;

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

    enum class ShadeRate : u8
    {
        R1x1 = 0,
        R2x2 = 3,
        R4x4 = 6,
    };

    struct ShadeRateMap
    {
        i32 tiles_x = 0, tiles_y = 0;
        i32 tile = 16;
        std::vector<u8> rate;

        bool empty() const { return rate.empty(); }
        u8 at(i32 tx, i32 ty) const
        {
            if (tx < 0 || ty < 0 || tx >= tiles_x || ty >= tiles_y) return (u8)ShadeRate::R1x1;
            return rate[(size_t)ty * (size_t)tiles_x + (size_t)tx];
        }
    };

    struct FlatItem
    {
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        f32 cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        bool smooth = false;
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

    struct Stats
    {
        u64 frames = 0;
        u64 instances_drawn = 0;
        u64 instances_skipped = 0;
        u64 core_pixels = 0;
        u64 shaded_pixels = 0;
        u64 pixels_skipped = 0;
        u64 hidden_instances = 0;

        u64 ns_track = 0;
        u64 ns_clear = 0;
        u64 ns_draw = 0;

        void reset() { *this = Stats{}; }
    };
}