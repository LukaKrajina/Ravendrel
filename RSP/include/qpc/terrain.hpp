#pragma once
//
// 高度场地形（Heightfield terrain）—— Genesis 对标能力之一
//
// ═══════════════════════════════════════════════════════════════════════
// 为什么需要
// ═══════════════════════════════════════════════════════════════════════
//
// 现有 qpc 只能用 AABB / Sphere / Capsule / Cylinder / ConvexHull 表达地面。
// 机器人仿真的实际场景（崎岖地形、楼梯、斜坡、随机障碍）无法用这些基本体
// 近似：要么用上千个盒子堆（碰撞检测 O(n log n) 但内存与建模成本高），
// 要么放弃与地形的真实接触（控制器学到"悬浮"行为，上真机即失效）。
//
// Genesis 用 gs.morphs.Terrain 在 build() 时把地形烘焙为
//   渲染用：三角网格
//   碰撞用：高度图 + SDF
// 本实现采用同一思路，并保持接口形态一致：
//
//   gs.morphs.Terrain(n_subterrains=(3,3), subterrain_size=(12,12),
//                     subterrain_types="[slope_terrain, stairs_terrain]",
//                     horizontal_scale=0.25, vertical_scale=0.005,
//                     randomize=True)
//   → TerrainSpec{n_subterrains, subterrain_size, subterrain_types,
//                 horizontal_scale, vertical_scale, randomize, seed}
//
// ───────────────────────────────────────────────────────────────────────
//
// 【与 Genesis 的差异，如实记录】
//
// 1. 只有**高度图**碰撞（height-vs-height 沿 Y 轴），没有 SDF。
//    代价：无法表达 overhang（洞穴/悬垂），也无法正确处理陡峭壁面。
//    收益：碰撞查询 O(1)（双线性插值），且对机器人仿真的典型地形
//    （斜坡/楼梯/起伏）已足够。这是刻意的取舍，不是遗漏。
//
// 2. 无 `Terrain(randomize=True)` 的**构建期**随机化。构建期随机化需要
//    固定的 seed 才能复现；本实现把随机化放在 TerrainSpec::seed 上，
//    语义等价但更显式。
//
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

    // ─── 子地形类型 ────────────────────────────────────────────────
    // 命名与 Genesis 的 gs.morphs 保持一致，便于迁移。
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

    // ─── TerrainSpec：地形描述（对应 gs.options.Terrain）────────────
    struct TerrainSpec
    {
        // 子地形网格布局：n_subterrains = (nx, ny)
        size_t nx = 1;
        size_t ny = 1;
        // 单个子地形的物理尺寸（米）
        double subterrain_size_x = 12.0;
        double subterrain_size_y = 12.0;
        // 每个子地形（按行优先）的类型
        std::vector<SubterrainType> subterrain_types{SubterrainType::Flat};
        // 水平采样间距（米）与高度量化步长（米）
        double horizontal_scale = 0.25;
        double vertical_scale = 0.005;
        // 随机化种子。固定 seed ⇒ 完全可复现的地形。
        uint64_t seed = 0;

        [[nodiscard]] double total_size_x() const { return subterrain_size_x * static_cast<double>(nx); }
        [[nodiscard]] double total_size_y() const { return subterrain_size_y * static_cast<double>(ny); }

        [[nodiscard]] size_t subterrain_count() const { return nx * ny; }

        // 校验参数一致性。错误在构造期暴露，而非在第一次碰撞查询时。
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

        // 按行优先取子地形类型；越界索引回绕到 0（Flat），
        // 避免 UB —— 越界只可能来自调用方笔误，不应让它变成未定义行为。
        [[nodiscard]] SubterrainType type_at(size_t ix, size_t iy) const
        {
            const size_t idx = iy * nx + ix;
            return idx < subterrain_types.size() ? subterrain_types[idx] : SubterrainType::Flat;
        }
    };

    // ─── Heightfield：烘焙后的高度场 ──────────────────────────────
    //
    // 网格分辨率按物理尺寸与 horizontal_scale 推出，故相同物理尺寸下
    // 内存占用与查询成本可控。采样用**双线性插值**：比最近邻光滑得多
    // （避免机器人踩台阶时被量化台阶弹飞），且仍是 O(1)。
    class Heightfield
    {
    public:
        Heightfield() = default;

        explicit Heightfield(const TerrainSpec &spec)
        {
            build(spec);
        }

        // 烘焙。幂等：重复调用会重新生成（并按相同 seed 得到同一结果）。
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

            // 每个子地形一个独立 RNG 流：某子地形的参数变化不影响其它子地形，
            // 这让"只改第 3 块地形"不会连带改变第 7 块的形状。
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

        // 世界坐标处的地面高度。超出范围则夹到边界（而非抛异常）——
        // 越界查询通常来自角色走出地图边缘，夹边比崩溃更符合仿真语义。
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
            // 双线性插值
            return (h00 * (1 - tx) + h10 * tx) * (1 - ty) +
                   (h01 * (1 - tx) + h11 * tx) * ty;
        }

        // 地面法线（中心差分，O(1)）。归一化；完全平坦时返回 +Y。
        [[nodiscard]] Vec3 normal_at(double x, double y) const
        {
            const double d = spec_.horizontal_scale;
            const double hx = height_at(x + d, y) - height_at(x - d, y);
            const double hy = height_at(x, y + d) - height_at(x, y - d);
            // n = (−dh/dx, 1, −dh/dy) × (2d) —— 缩放对归一化无影响
            return Vec3(-hx, 2.0 * d, -hy).normalized();
        }

        // 地形上的最高点与最低点（供 spawner 把机器人放在安全高度）
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

        // 把高度量化到 vertical_scale 网格。
        // 与 Genesis 一致：渲染网格与碰撞高度图因此完全对齐，
        // 不会出现"看得见台阶但踩不到"。
        void quantize_vertical()
        {
            const double q = spec_.vertical_scale;
            if (q <= 0.0)
                return;
            for (double &h : heights_)
                h = std::round(h / q) * q;
        }

        // 导出高度图（Genesis 的 .gstraj 思路的简化版：纯数据，可复现）
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

        // 子地形局部归一化坐标 ∈ [−1, 1]，用于各生成器的形状定义
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
            // 从本子地形的 RNG 抽一个 salt，并入下方按格点播种的哈希。
            // 若不引入它，RandomUniform / DiscreteObstacles / SteppingStones
            // 的形状只由格点坐标决定 —— 那样 TerrainSpec::seed 会**完全
            // 失效**，不同 seed 生成同一片地形（确定性测试即会捕获）。
            const uint64_t salt = rng();
            // 该子地形覆盖的全局网格范围
            const double x0 = -0.5 * spec_.total_size_x() +
                              static_cast<double>(ix) * spec_.subterrain_size_x;
            const double y0 = -0.5 * spec_.total_size_y() +
                              static_cast<double>(iy) * spec_.subterrain_size_y;
            const size_t cx0 = static_cast<size_t>(std::max(0.0, (x0 + 0.5 * spec_.total_size_x()) / spec_.horizontal_scale));
            const size_t cy0 = static_cast<size_t>(std::max(0.0, (y0 + 0.5 * spec_.total_size_y()) / spec_.horizontal_scale));
            const size_t cx1 = std::min(cols_ - 1, cx0 + static_cast<size_t>(spec_.subterrain_size_x / spec_.horizontal_scale) + 1);
            const size_t cy1 = std::min(rows_ - 1, cy0 + static_cast<size_t>(spec_.subterrain_size_y / spec_.horizontal_scale) + 1);

            // 台阶参数（Stairs / PyramidStairs 共用）
            const int step_count = 8 + static_cast<int>(rng() % 4); // 8..11 级
            const double step_height = 0.08;
            const int obstacle_count = 4 + static_cast<int>(rng() % 8);
            const double obstacle_height = 0.15;
            // 阶梯石半径/数量（SteppingStones）
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

        // salt 参与按格点的哈希，使同一坐标在不同 seed 下得到不同高度，
        // 同时保证同一 (seed, 格点) 的高度稳定可复现。
        [[nodiscard]] static double generate(SubterrainType type, double u, double v,
                                             uint64_t salt, int step_count,
                                             double step_height, int obstacle_count,
                                             double obstacle_height, int stone_count,
                                             double stone_radius)
        {
            auto hash = [salt](double a, double b) {
                // 把局部坐标量化到格点索引后与 salt 混合
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
                // 离散块状随机起伏：比连续噪声更"可踩"，也避免机器人在
                // 无限平滑斜面上学到没有意义的精细平衡。
                // 量化到 4×4 格，每格高度由 hash(u,v,salt) 决定 →
                // 同一 seed 下同一格点高度稳定（地形不抖动）。
                const double cu = std::floor((u + 1.0) * 2.0);
                const double cv = std::floor((v + 1.0) * 2.0);
                std::uniform_real_distribution<double> dd(-0.2, 0.2);
                std::mt19937_64 local(hash(cu, cv));
                return dd(local);
            }

            case SubterrainType::Sloped:
                // 沿 u 方向的斜面；高度随 u 单调
                return 0.3 * u;

            case SubterrainType::PyramidSloped:
                // 四面锥：取 |u|,|v| 的最大值作为斜率方向
                return 0.4 * std::max(std::fabs(u), std::fabs(v)) - 0.2;

            case SubterrainType::Wave:
                // 两个正弦叠加：产生可分辨的波峰波谷，而非高频噪声
                return 0.15 * std::sin(3.0 * u) + 0.10 * std::sin(3.0 * v);

            case SubterrainType::Stairs:
            {
                // 沿 u 方向上行台阶；floor 保证离散高度
                const double t = (u + 1.0) * 0.5; // [0,1]
                const int idx = static_cast<int>(t * step_count);
                const int clamped = std::clamp(idx, 0, step_count);
                return clamped * step_height;
            }

            case SubterrainType::PyramidStairs:
            {
                // 沿对角线方向的阶梯金字塔
                const double t = (u + v + 2.0) * 0.25; // [0,1]
                const int clamped = std::clamp(static_cast<int>(t * step_count), 0, step_count);
                return clamped * step_height;
            }

            case SubterrainType::DiscreteObstacles:
            {
                // 稀疏箱形障碍：高度取决于所在网格是否被障碍占用
                const double gu = (u + 1.0) * 0.5 * obstacle_count;
                const double gv = (v + 1.0) * 0.5 * obstacle_count;
                std::uniform_real_distribution<double> d(0.0, 1.0);
                std::mt19937_64 local(hash(std::floor(gu), std::floor(gv)));
                return (d(local) < 0.3) ? obstacle_height : 0.0;
            }

            case SubterrainType::SteppingStones:
            {
                // 圆盘状石块：中心附近抬起，边缘落回 0。
                // 石块中心用 hash 抖动，使不同 seed 下布局不同。
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
                // 平顶：中心满高，边缘平滑过渡（避免不连续法线）
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

    // ─── 与 PhysicsKernel 的接线 ────────────────────────────────────
    //
    // 高度场本身不参与碰撞检测（那需要 4×4 的窄相），而是作为**地形查询
    // 提供者**：控制器/环境用它计算支撑高度与法线，从而：
    //   1. 把机器人放到地面上（spawn）
    //   2. 施加支撑力（行走/站立）
    //   3. 计算接触奖励（足部触地判定）
    //
    // 这与 Genesis 的做法一致：Terrain 不是碰撞体，而是碰撞查询的数据源。
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

        // 把位于 (x, z)、半径 r 的球体放到地形上方。
        // 返回期望的球心 y 坐标。
        [[nodiscard]] double place_sphere(double x, double z, double r) const
        {
            return field_.height_at(x, z) + r;
        }

        // 支撑力：把球体压向地形。stiffness 越大越硬。
        // 超过 max_force 时截断，避免数值爆炸 —— 这是稳定性与
        // "过刚导致发散"之间的显式取舍。
        [[nodiscard]] Vec3 support_force(double x, double y, double z, double r,
                                         double stiffness = 800.0,
                                         double max_force = 2000.0) const
        {
            const double ground = field_.height_at(x, z);
            const double pen = ground - (y - r); // >0 表示陷入地面
            if (pen <= 0.0)
                return Vec3{};
            double f = stiffness * pen;
            if (f > max_force)
                f = max_force;
            // 法线方向修正：斜坡上支撑力应有水平分量
            const Vec3 n = field_.normal_at(x, z);
            return Vec3(n.x * f, n.y * f, n.z * f);
        }

        [[nodiscard]] const Heightfield &field() const { return field_; }
        [[nodiscard]] Heightfield &field() { return field_; }

    private:
        Heightfield field_;
    };

} // namespace rvdlrsp::qpc
