#pragma once
#include "core/context.hpp"
#include "draw/instance.hpp"

#include "utils/Pa.hpp"

namespace fox
{
    class CpuBackend
    {
    public:
        bool init(int w, int h);

        bool resize(int w, int h);

        void shutdown();

        void render(const DisplayList &dl, Context &ctx);

        const u8 *pixels() const { return color_.data(); }
        u8 *pixels() { return color_.data(); }
        int width() const { return width_; }
        int height() const { return height_; }
        size_t stride() const { return (size_t)width_ * 4; }

        size_t non_background_pixels() const;

        bool write_ppm(const char *path) const;

        u32 last_instance_count() const { return (u32)instances_.instances.size(); }

        void set_glass_supported(bool on) { glass_ = on; }
        bool glass_supported() const { return glass_; }

        void set_accel(bool on) { accel_ = on; }
        bool accel() const { return accel_; }

        const pa::Stats &stats() const { return stats_; }
        void reset_stats() { stats_.reset(); }
        const pa::DamageTracker &damage() const { return damage_; }

    private:
        void raster(const InstanceSet &set, Context &ctx, f32 scale);
        void blend(int x, int y, f32 r, f32 g, f32 b, f32 a);

        std::vector<u8> color_;
        InstanceSet instances_;
        int width_ = 0;
        int height_ = 0;
        bool glass_ = true;

        pa::DamageTracker damage_;
        std::vector<pa::TileRect> tile_tmp_;
        pa::Stats stats_;
        bool accel_ = true;
    };
}