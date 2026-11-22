#pragma once
#include "math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace rvdlrsp::qpc
{

    enum class SubterrainType
    {
        Flat = 0,
        RandomUniform,
        Sloped,
        PyramidSloped,
        DiscreteObstacles,
        Wave,
        Stairs,
        PyramidStairs,
        SteppingStones,
    };

    inline const char *subterrain_name(SubterrainType t)
    {
        switch (t)
        {
        case SubterrainType::Flat:               return "flat_terrain";
        case SubterrainType::RandomUniform:       return "random_uniform_terrain";
        case SubterrainType::Sloped:              return "sloped_terrain";
        case SubterrainType::PyramidSloped:       return "pyramid_sloped_terrain";
        case SubterrainType::DiscreteObstacles:   return "discrete_obstacles_terrain";
        case SubterrainType::Wave:                return "wave_terrain";
        case SubterrainType::Stairs:              return "stairs_terrain";
        case SubterrainType::PyramidStairs:       return "pyramid_stairs_terrain";
        case SubterrainType::SteppingStones:      return "stepping_stones_terrain";
        }
        return "unknown";
    }

    inline const char *subterrain_type_by_name(const char *name)
    {
        for (int i = 0; i <= static_cast<int>(SubterrainType::SteppingStones); ++i)
        {
            const auto t = static_cast<SubterrainType>(i);
            if (std::string(subterrain_name(t)) == name)
                return subterrain_name(t);
        }
        return nullptr;
    }

    struct TerrainSpec
    {
        size_t nx = 1;
        size_t ny = 1;
        double subterrain_size_x = 12.0;
        double subterrain_size_y = 12.0;
        std::vector<SubterrainType> subterrain_types{SubterrainType::Flat};
        double horizontal_scale = 0.25;
        double vertical_scale = 0.005;
        uint64_t seed = 0;

        [[nodiscard]] double total_size_x() const { return subterrain_size_x * static_cast<double>(nx); }
        [[nodiscard]] double total_size_y() const { return subterrain_size_y * static_cast<double>(ny); }

        [[nodiscard]] size_t subterrain_count() const { return nx * ny; }

        void validate() const
        {
            if (nx == 0 || ny == 0)
                throw std::invalid_argument("[qpc.TerrainSpec] nx/ny must be positive");
            if (subterrain_size_x <= 0.0 || subterrain_size_y <= 0.0)
                throw std::invalid_argument("[qpc.TerrainSpec] subterrain_size must be positive");
            if (horizontal_scale <= 0.0)
                throw std::invalid_argument("[qpc.TerrainSpec] horizontal_scale must be positive");
            if (vertical_scale <= 0.0)
                throw std::invalid_argument("[qpc.TerrainSpec] vertical_scale must be positive");
            if (subterrain_types.size() != subterrain_count())
                throw std::invalid_argument(
                    "[qpc.TerrainSpec] subterrain_types.size() != nx*ny");
        }

        [[nodiscard]] SubterrainType type_at(size_t ix, size_t iy) const
        {
            const size_t idx = iy * nx + ix;
            return idx < subterrain_types.size() ? subterrain_types[idx] : SubterrainType::Flat;
        }
    };

    class Heightfield
    {
    public:
        Heightfield() = default;

        explicit Heightfield(const TerrainSpec &spec)
        {
            build(spec);
        }

        void build(const TerrainSpec &spec)
        {
            spec.validate();
            spec_ = spec;

            cols_ = static_cast<size_t>(spec.total_size_x() / spec.horizontal_scale) + 1;
            rows_ = static_cast<size_t>(spec.total_size_y() / spec.horizontal_scale) + 1;
            if (cols_ < 2)
                cols_ = 2;
            if (rows_ < 2)
                rows_ = 2;

            heights_.assign(cols_ * rows_, 0.0);

            for (size_t iy = 0; iy < spec.ny; ++iy)
            {
                for (size_t ix = 0; ix < spec.nx; ++ix)
                {
                    std::mt19937_64 rng(spec.seed + static_cast<uint64_t>(iy) * spec.nx + ix);
                    bake_subterrain(spec.type_at(ix, iy), ix, iy, rng);
                }
            }
        }

        [[nodiscard]] size_t rows() const { return rows_; }
        [[nodiscard]] size_t cols() const { return cols_; }
        [[nodiscard]] const TerrainSpec &spec() const { return spec_; }

        [[nodiscard]] double height_at(double x, double y) const
        {
            if (heights_.empty())
                return 0.0;
            const double fx = (x + 0.5 * spec_.total_size_x()) / spec_.horizontal_scale;
            const double fy = (y + 0.5 * spec_.total_size_y()) / spec_.horizontal_scale;
            const double cx = std::clamp(fx, 0.0, static_cast<double>(cols_ - 1));
            const double cy = std::clamp(fy, 0.0, static_cast<double>(rows_ - 1));
            const size_t x0 = static_cast<size_t>(cx);
            const size_t y0 = static_cast<size_t>(cy);
            const size_t x1 = std::min(x0 + 1, cols_ - 1);
            const size_t y1 = std::min(y0 + 1, rows_ - 1);
            const double tx = cx - static_cast<double>(x0);
            const double ty = cy - static_cast<double>(y0);

            const double h00 = at(x0, y0), h10 = at(x1, y0);
            const double h01 = at(x0, y1), h11 = at(x1, y1);
            return (h00 * (1 - tx) + h10 * tx) * (1 - ty) +
                   (h01 * (1 - tx) + h11 * tx) * ty;
        }

        [[nodiscard]] Vec3 normal_at(double x, double y) const
        {
            const double d = spec_.horizontal_scale;
            const double hx = height_at(x + d, y) - height_at(x - d, y);
            const double hy = height_at(x, y + d) - height_at(x, y - d);
            return Vec3(-hx, 2.0 * d, -hy).normalized();
        }

        [[nodiscard]] double max_height() const
        {
            return heights_.empty()
                       ? 0.0
                       : *std::max_element(heights_.begin(), heights_.end());
        }
        [[nodiscard]] double min_height() const
        {
            return heights_.empty()
                       ? 0.0
                       : *std::min_element(heights_.begin(), heights_.end());
        }

        void quantize_vertical()
        {
            const double q = spec_.vertical_scale;
            if (q <= 0.0)
                return;
            for (double &h : heights_)
                h = std::round(h / q) * q;
        }

        [[nodiscard]] std::vector<double> data() const { return heights_; }
        void load_data(const std::vector<double> &h)
        {
            if (h.size() != heights_.size())
                throw std::invalid_argument(
                    "[qpc.Heightfield] data size mismatch");
            heights_ = h;
        }

    private:
        [[nodiscard]] double at(size_t x, size_t y) const
        {
            return heights_[y * cols_ + x];
        }
        void set_at(size_t x, size_t y, double h)
        {
            heights_[y * cols_ + x] = h;
        }

        [[nodiscard]] double local_u(double x, size_t ix) const
        {
            const double x0 = -0.5 * spec_.total_size_x() +
                              static_cast<double>(ix) * spec_.subterrain_size_x;
            const double half = 0.5 * spec_.subterrain_size_x;
            return std::clamp((x - x0) / half, -1.0, 1.0);
        }
        [[nodiscard]] double local_v(double y, size_t iy) const
        {
            const double y0 = -0.5 * spec_.total_size_y() +
                              static_cast<double>(iy) * spec_.subterrain_size_y;
            const double half = 0.5 * spec_.subterrain_size_y;
            return std::clamp((y - y0) / half, -1.0, 1.0);
        }

        void bake_subterrain(SubterrainType type, size_t ix, size_t iy,
                             std::mt19937_64 &rng)
        {
            const uint64_t salt = rng();
            const double x0 = -0.5 * spec_.total_size_x() +
                              static_cast<double>(ix) * spec_.subterrain_size_x;
            const double y0 = -0.5 * spec_.total_size_y() +
                              static_cast<double>(iy) * spec_.subterrain_size_y;
            const size_t cx0 = static_cast<size_t>(std::max(0.0, (x0 + 0.5 * spec_.total_size_x()) / spec_.horizontal_scale));
            const size_t cy0 = static_cast<size_t>(std::max(0.0, (y0 + 0.5 * spec_.total_size_y()) / spec_.horizontal_scale));
            const size_t cx1 = std::min(cols_ - 1, cx0 + static_cast<size_t>(spec_.subterrain_size_x / spec_.horizontal_scale) + 1);
            const size_t cy1 = std::min(rows_ - 1, cy0 + static_cast<size_t>(spec_.subterrain_size_y / spec_.horizontal_scale) + 1);

            const int step_count = 8 + static_cast<int>(rng() % 4);
            const double step_height = 0.08;
            const int obstacle_count = 4 + static_cast<int>(rng() % 8);
            const double obstacle_height = 0.15;
            const int stone_count = 6 + static_cast<int>(rng() % 6);
            const double stone_radius = 0.22;

            for (size_t gy = cy0; gy <= cy1; ++gy)
            {
                for (size_t gx = cx0; gx <= cx1; ++gx)
                {
                    const double x = gx * spec_.horizontal_scale - 0.5 * spec_.total_size_x();
                    const double y = gy * spec_.horizontal_scale - 0.5 * spec_.total_size_y();
                    const double u = local_u(x, ix);
                    const double v = local_v(y, iy);
                    set_at(gx, gy, generate(type, u, v, salt, step_count, step_height,
                                             obstacle_count, obstacle_height,
                                             stone_count, stone_radius));
                }
            }
        }

        [[nodiscard]] static double generate(SubterrainType type, double u, double v,
                                             uint64_t salt, int step_count,
                                             double step_height, int obstacle_count,
                                             double obstacle_height, int stone_count,
                                             double stone_radius)
        {
            auto hash = [salt](double a, double b) {
                const uint64_t ia = static_cast<uint64_t>(std::llround(a * 1024.0));
                const uint64_t ib = static_cast<uint64_t>(std::llround(b * 1024.0));
                uint64_t h = salt;
                h ^= ia * 0x9E3779B97F4A7C15ULL;
                h ^= ib * 0xC2B2AE3D27D4EB4FULL;
                h ^= h >> 29;
                h *= 0xBF58476D1CE4E5B9ULL;
                h ^= h >> 32;
                return h;
            };
            switch (type)
            {
            case SubterrainType::Flat:
                return 0.0;

            case SubterrainType::RandomUniform:
            {
                const double cu = std::floor((u + 1.0) * 2.0);
                const double cv = std::floor((v + 1.0) * 2.0);
                std::uniform_real_distribution<double> dd(-0.2, 0.2);
                std::mt19937_64 local(hash(cu, cv));
                return dd(local);
            }

            case SubterrainType::Sloped:
                return 0.3 * u;

            case SubterrainType::PyramidSloped:
                return 0.4 * std::max(std::fabs(u), std::fabs(v)) - 0.2;

            case SubterrainType::Wave:
                return 0.15 * std::sin(3.0 * u) + 0.10 * std::sin(3.0 * v);

            case SubterrainType::Stairs:
            {
                const double t = (u + 1.0) * 0.5;
                const int idx = static_cast<int>(t * step_count);
                const int clamped = std::clamp(idx, 0, step_count);
                return clamped * step_height;
            }

            case SubterrainType::PyramidStairs:
            {
                const double t = (u + v + 2.0) * 0.25;
                const int clamped = std::clamp(static_cast<int>(t * step_count), 0, step_count);
                return clamped * step_height;
            }

            case SubterrainType::DiscreteObstacles:
            {
                const double gu = (u + 1.0) * 0.5 * obstacle_count;
                const double gv = (v + 1.0) * 0.5 * obstacle_count;
                std::uniform_real_distribution<double> d(0.0, 1.0);
                std::mt19937_64 local(hash(std::floor(gu), std::floor(gv)));
                return (d(local) < 0.3) ? obstacle_height : 0.0;
            }

            case SubterrainType::SteppingStones:
            {
                const double pu = (u + 1.0) * 0.5 * stone_count;
                const double pv = (v + 1.0) * 0.5 * stone_count;
                const double su = std::floor(pu);
                const double sv = std::floor(pv);
                std::uniform_real_distribution<double> jitter(-0.3, 0.3);
                std::mt19937_64 local(hash(su, sv));
                const double cx = (su + 0.5 + jitter(local)) / stone_count * 2.0 - 1.0;
                const double cy = (sv + 0.5 + jitter(local)) / stone_count * 2.0 - 1.0;
                const double d = std::sqrt((u - cx) * (u - cx) + (v - cy) * (v - cy));
                if (d > stone_radius)
                    return 0.0;
                const double t = 1.0 - d / stone_radius;
                return obstacle_height * std::min(1.0, t * 3.0);
            }
            }
            return 0.0;
        }

        TerrainSpec spec_{};
        size_t rows_ = 0;
        size_t cols_ = 0;
        std::vector<double> heights_;
    };

    class TerrainBody
    {
    public:
        TerrainBody() = default;
        explicit TerrainBody(Heightfield field) : field_(std::move(field)) {}

        [[nodiscard]] double support_height(double x, double z) const
        {
            return field_.height_at(x, z);
        }
        [[nodiscard]] Vec3 support_normal(double x, double z) const
        {
            return field_.normal_at(x, z);
        }

        [[nodiscard]] double place_sphere(double x, double z, double r) const
        {
            return field_.height_at(x, z) + r;
        }

        [[nodiscard]] Vec3 support_force(double x, double y, double z, double r,
                                         double stiffness = 800.0,
                                         double max_force = 2000.0) const
        {
            const double ground = field_.height_at(x, z);
            const double pen = ground - (y - r);
            if (pen <= 0.0)
                return Vec3{};
            double f = stiffness * pen;
            if (f > max_force)
                f = max_force;
            const Vec3 n = field_.normal_at(x, z);
            return Vec3(n.x * f, n.y * f, n.z * f);
        }

        [[nodiscard]] const Heightfield &field() const { return field_; }
        [[nodiscard]] Heightfield &field() { return field_; }

    private:
        Heightfield field_;
    };

}