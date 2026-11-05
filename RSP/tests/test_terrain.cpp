// 高度场地形单元测试
#include "test_framework.hpp"
#include "qpc/terrain.hpp"

#include <cmath>

using namespace rvdlrsp::qpc;

// ═══ 规格校验 ═════════════════════════════════════════════════════

QTEST(terrain_spec_validation) {
    TerrainSpec s;
    s.validate(); // 默认值合法

    bool threw = false;
    try { TerrainSpec bad; bad.nx = 0; bad.validate(); }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // nx = 0 必须被拒

    threw = false;
    try { TerrainSpec bad; bad.horizontal_scale = 0.0; bad.validate(); }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);

    threw = false;
    try
    {
        TerrainSpec bad;
        bad.nx = 2;
        bad.ny = 2;
        bad.subterrain_types = {SubterrainType::Flat}; // 只 1 个，却要 4 个
        bad.validate();
    }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // 类型数量与网格不匹配

    // 尺寸计算
    TerrainSpec g;
    g.nx = 3;
    g.ny = 2;
    g.subterrain_size_x = 10.0;
    g.subterrain_size_y = 4.0;
    g.subterrain_types.assign(6, SubterrainType::Flat);
    QCHECK_NEAR(g.total_size_x(), 30.0, 1e-12);
    QCHECK_NEAR(g.total_size_y(), 8.0, 1e-12);
    QCHECK(g.subterrain_count() == 6);
    // 行优先索引：type_at(ix, iy) → types[iy*nx + ix]
    g.subterrain_types[4] = SubterrainType::Stairs; // (1,1) → 1*3+1 = 4
    QCHECK(g.type_at(1, 1) == SubterrainType::Stairs);
    QCHECK(g.type_at(1, 0) == SubterrainType::Flat);
    // 越界索引回绕而非 UB
    QCHECK(g.type_at(99, 99) == SubterrainType::Flat);
}

QTEST(terrain_type_names_roundtrip) {
    // 与 Genesis gs.morphs 的命名保持一致，便于迁移
    QCHECK(std::string(subterrain_name(SubterrainType::Flat)) == "flat_terrain");
    QCHECK(std::string(subterrain_name(SubterrainType::Sloped)) == "sloped_terrain");
    QCHECK(std::string(subterrain_name(SubterrainType::Stairs)) == "stairs_terrain");
    for (int i = 0; i <= static_cast<int>(SubterrainType::SteppingStones); ++i)
    {
        const auto t = static_cast<SubterrainType>(i);
        const char *n = subterrain_name(t);
        QCHECK(n != nullptr && std::string(n) != "unknown");
        QCHECK(std::string(subterrain_type_by_name(n)) == n);
    }
    QCHECK(subterrain_type_by_name("no_such_terrain") == nullptr);
}

// ═══ 烘焙与采样 ════════════════════════════════════════════════════

QTEST(terrain_flat_is_flat) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Flat};
    s.horizontal_scale = 0.5;
    Heightfield f(s);
    // 处处为 0
    for (double x = -4.0; x <= 4.0; x += 0.7)
        for (double y = -4.0; y <= 4.0; y += 0.7)
            QCHECK_NEAR(f.height_at(x, y), 0.0, 1e-12);
    QCHECK_NEAR(f.max_height(), 0.0, 1e-12);
    QCHECK_NEAR(f.min_height(), 0.0, 1e-12);
    // 平地法线 = +Y
    const Vec3 n = f.normal_at(1.0, 1.0);
    QCHECK_NEAR(n.y, 1.0, 1e-9);
    QCHECK_NEAR(n.x, 0.0, 1e-9);
    QCHECK_NEAR(n.z, 0.0, 1e-9);
}

QTEST(terrain_sloped_monotonic) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Sloped};
    s.horizontal_scale = 0.25;
    Heightfield f(s);
    // 斜面沿 x 单调递增（生成器用 u，故 x 越大越高）
    double prev = f.height_at(-5.0, 0.0);
    bool monotonic = true;
    for (double x = -5.0; x <= 5.0; x += 0.5)
    {
        const double h = f.height_at(x, 0.0);
        if (h < prev - 1e-9)
            monotonic = false;
        prev = h;
    }
    QCHECK(monotonic);
    // 斜面法线不应与 +Y 平行
    const Vec3 n = f.normal_at(0.0, 0.0);
    QCHECK(std::fabs(n.x) > 1e-3);
}

QTEST(terrain_stairs_are_discrete) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Stairs};
    s.horizontal_scale = 0.1;
    s.vertical_scale = 0.005;
    Heightfield f(s);
    // 台阶高度必须取离散值（量化后）
    f.quantize_vertical();
    // 沿 x 走一遍，高度应单调不减
    double prev = f.height_at(-5.0, 0.0);
    for (double x = -5.0; x <= 5.0; x += 0.2)
    {
        const double h = f.height_at(x, 0.0);
        QCHECK(h >= prev - 1e-6);
        prev = h;
    }
    // 最高台阶应 > 0
    QCHECK(f.max_height() > 0.1);
}

QTEST(terrain_quantize_vertical) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::RandomUniform};
    s.vertical_scale = 0.1;
    Heightfield f(s);
    f.quantize_vertical();
    // 所有高度都应是 0.1 的整数倍
    for (double h : f.data())
    {
        const double k = h / 0.1;
        QCHECK_NEAR(k, std::round(k), 1e-6);
    }
    bool threw = false;
    try { f.load_data({1.0}); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw); // 尺寸不符必须拒绝
}

QTEST(terrain_determinism) {
    // 同一 seed 必须产生**完全相同**的地形 —— 这是训练可复现的前提
    TerrainSpec s;
    s.nx = 2;
    s.ny = 2;
    s.subterrain_types.assign(4, SubterrainType::RandomUniform);
    s.seed = 12345;
    s.horizontal_scale = 0.25;

    Heightfield a(s);
    Heightfield b(s);
    QCHECK(a.data() == b.data()); // 逐位相同，不只是近似

    // 不同 seed 应产生不同地形
    TerrainSpec t = s;
    t.seed = 999;
    Heightfield c(t);
    QCHECK(c.data() != a.data());
}

QTEST(terrain_mixed_subterrain_blocks_independent) {
    // 每个子地形用独立 RNG 流：改第 1 块不应影响第 2 块
    TerrainSpec s;
    s.nx = 2;
    s.ny = 1;
    s.subterrain_types = {SubterrainType::RandomUniform, SubterrainType::RandomUniform};
    s.seed = 7;
    s.horizontal_scale = 0.5;
    Heightfield a(s);

    TerrainSpec t = s;
    t.subterrain_types[0] = SubterrainType::Flat;
    Heightfield b(t);

    // 右边那块（第 1 号，x > 0）应完全不变
    double diff = 0.0;
    for (double x = 0.1; x < 5.9; x += 0.5)
    {
        const double h0 = a.height_at(x, 0.0);
        const double h1 = b.height_at(x, 0.0);
        diff += std::fabs(h0 - h1);
    }
    QCHECK(diff < 1e-12);
    // 左边那块应变了
    QCHECK(std::fabs(a.height_at(-5.0, 0.0) - b.height_at(-5.0, 0.0)) > 0.0);
}

QTEST(terrain_interpolation_smooth) {
    // 双线性插值应产生介于相邻网格点之间的值，且连续
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Wave};
    s.horizontal_scale = 0.5;
    Heightfield f(s);
    // 采样应连续：很小的位置变化不应引起大的高度跳变
    double max_jump = 0.0;
    double h = f.height_at(-3.0, 0.0);
    for (double x = -3.0; x < 3.0; x += 0.001)
    {
        const double nh = f.height_at(x, 0.0);
        max_jump = std::max(max_jump, std::fabs(nh - h));
        h = nh;
    }
    // 网格间距 0.5，步长 0.001 → 每步高度变化应远小于总高差
    QCHECK(max_jump < 0.05);
}

QTEST(terrain_bounds_clamp) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Wave};
    s.horizontal_scale = 0.5;
    Heightfield f(s);
    // 越界查询应夹到边界而非抛异常或 UB
    const double far = f.height_at(1000.0, 1000.0);
    const double edge = f.height_at(0.5 * s.total_size_x() - 0.001,
                                    0.5 * s.total_size_y() - 0.001);
    QCHECK(std::isfinite(far));
    QCHECK_NEAR(far, edge, 1e-9); // 夹到边界
    QCHECK(std::isfinite(f.normal_at(1e6, -1e6).x));
}

QTEST(terrain_normal_points_up) {
    // 所有地形法线的 Y 分量都应为正（重力朝 −Y，地面法线朝上）
    for (auto type : {SubterrainType::Flat, SubterrainType::Sloped,
                      SubterrainType::PyramidSloped, SubterrainType::Wave,
                      SubterrainType::Stairs, SubterrainType::PyramidStairs,
                      SubterrainType::DiscreteObstacles,
                      SubterrainType::SteppingStones})
    {
        TerrainSpec s;
        s.subterrain_types = {type};
        s.horizontal_scale = 0.25;
        Heightfield f(s);
        bool ok = true;
        for (double x = -4.0; x <= 4.0; x += 1.0)
            for (double y = -4.0; y <= 4.0; y += 1.0)
            {
                const Vec3 n = f.normal_at(x, y);
                if (n.y <= 0.0 || !std::isfinite(n.y))
                    ok = false;
            }
        QCHECK(ok);
    }
}

// ═══ TerrainBody：与物理内核的接线 ════════════════════════════════

QTEST(terrain_body_support) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Flat};
    s.horizontal_scale = 0.5;
    // 显式构造中间变量：TerrainBody tb(Heightfield(s)); 会被解析成
    // 「声明一个名为 s 的 Heightfield 参数」（最令人烦恼的解析），
    // 从而把 tb 变成函数而非对象。
    const Heightfield field(s);
    TerrainBody tb(field);

    QCHECK_NEAR(tb.support_height(1.0, 2.0), 0.0, 1e-12);
    // 半径 r 的球放在地面上 → 球心 y = r
    QCHECK_NEAR(tb.place_sphere(1.0, 2.0, 0.5), 0.5, 1e-12);

    // 球悬空 → 无支撑力
    QCHECK_NEAR(tb.support_force(0.0, 5.0, 0.0, 0.5).length(), 0.0, 1e-12);
    // 球陷入地面 → 有支撑力，方向朝上
    const Vec3 f = tb.support_force(0.0, 0.2, 0.0, 0.5);
    QCHECK(f.y > 0.0);
    // 陷入越深力越大
    const Vec3 f2 = tb.support_force(0.0, -1.0, 0.0, 0.5);
    QCHECK(f2.y > f.y);
    // max_force 截断：极深穿透不得产生爆炸力
    const Vec3 f3 = tb.support_force(0.0, -1e6, 0.0, 0.5);
    QCHECK(std::isfinite(f3.y));
    QCHECK(f3.y <= 2000.0 + 1e-6);
}

QTEST(terrain_body_sloped_support_has_lateral_component) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Sloped};
    s.horizontal_scale = 0.25;
    const Heightfield field(s);
    TerrainBody tb(field);
    // 斜坡上陷入 → 支撑力应有水平分量（否则球会沿坡面滑下）
    const Vec3 f = tb.support_force(0.0, -0.3, 0.0, 0.2);
    QCHECK(std::fabs(f.x) > 1e-6);
    QCHECK(f.y > 0.0);
}
