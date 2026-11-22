#include "test_framework.hpp"
#include "qpc/terrain.hpp"

#include <cmath>

using namespace rvdlrsp::qpc;


QTEST(terrain_spec_validation) {
    TerrainSpec s;
    s.validate();

    bool threw = false;
    try { TerrainSpec bad; bad.nx = 0; bad.validate(); }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);

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
        bad.subterrain_types = {SubterrainType::Flat};
        bad.validate();
    }
    catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);

    TerrainSpec g;
    g.nx = 3;
    g.ny = 2;
    g.subterrain_size_x = 10.0;
    g.subterrain_size_y = 4.0;
    g.subterrain_types.assign(6, SubterrainType::Flat);
    QCHECK_NEAR(g.total_size_x(), 30.0, 1e-12);
    QCHECK_NEAR(g.total_size_y(), 8.0, 1e-12);
    QCHECK(g.subterrain_count() == 6);
    g.subterrain_types[4] = SubterrainType::Stairs;
    QCHECK(g.type_at(1, 1) == SubterrainType::Stairs);
    QCHECK(g.type_at(1, 0) == SubterrainType::Flat);
    QCHECK(g.type_at(99, 99) == SubterrainType::Flat);
}

QTEST(terrain_type_names_roundtrip) {
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


QTEST(terrain_flat_is_flat) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Flat};
    s.horizontal_scale = 0.5;
    Heightfield f(s);
    for (double x = -4.0; x <= 4.0; x += 0.7)
        for (double y = -4.0; y <= 4.0; y += 0.7)
            QCHECK_NEAR(f.height_at(x, y), 0.0, 1e-12);
    QCHECK_NEAR(f.max_height(), 0.0, 1e-12);
    QCHECK_NEAR(f.min_height(), 0.0, 1e-12);
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
    const Vec3 n = f.normal_at(0.0, 0.0);
    QCHECK(std::fabs(n.x) > 1e-3);
}

QTEST(terrain_stairs_are_discrete) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Stairs};
    s.horizontal_scale = 0.1;
    s.vertical_scale = 0.005;
    Heightfield f(s);
    f.quantize_vertical();
    double prev = f.height_at(-5.0, 0.0);
    for (double x = -5.0; x <= 5.0; x += 0.2)
    {
        const double h = f.height_at(x, 0.0);
        QCHECK(h >= prev - 1e-6);
        prev = h;
    }
    QCHECK(f.max_height() > 0.1);
}

QTEST(terrain_quantize_vertical) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::RandomUniform};
    s.vertical_scale = 0.1;
    Heightfield f(s);
    f.quantize_vertical();
    for (double h : f.data())
    {
        const double k = h / 0.1;
        QCHECK_NEAR(k, std::round(k), 1e-6);
    }
    bool threw = false;
    try { f.load_data({1.0}); } catch (const std::invalid_argument &) { threw = true; }
    QCHECK(threw);
}

QTEST(terrain_determinism) {
    TerrainSpec s;
    s.nx = 2;
    s.ny = 2;
    s.subterrain_types.assign(4, SubterrainType::RandomUniform);
    s.seed = 12345;
    s.horizontal_scale = 0.25;

    Heightfield a(s);
    Heightfield b(s);
    QCHECK(a.data() == b.data());

    TerrainSpec t = s;
    t.seed = 999;
    Heightfield c(t);
    QCHECK(c.data() != a.data());
}

QTEST(terrain_mixed_subterrain_blocks_independent) {
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

    double diff = 0.0;
    for (double x = 0.1; x < 5.9; x += 0.5)
    {
        const double h0 = a.height_at(x, 0.0);
        const double h1 = b.height_at(x, 0.0);
        diff += std::fabs(h0 - h1);
    }
    QCHECK(diff < 1e-12);
    QCHECK(std::fabs(a.height_at(-5.0, 0.0) - b.height_at(-5.0, 0.0)) > 0.0);
}

QTEST(terrain_interpolation_smooth) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Wave};
    s.horizontal_scale = 0.5;
    Heightfield f(s);
    double max_jump = 0.0;
    double h = f.height_at(-3.0, 0.0);
    for (double x = -3.0; x < 3.0; x += 0.001)
    {
        const double nh = f.height_at(x, 0.0);
        max_jump = std::max(max_jump, std::fabs(nh - h));
        h = nh;
    }
    QCHECK(max_jump < 0.05);
}

QTEST(terrain_bounds_clamp) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Wave};
    s.horizontal_scale = 0.5;
    Heightfield f(s);
    const double far = f.height_at(1000.0, 1000.0);
    const double edge = f.height_at(0.5 * s.total_size_x() - 0.001,
                                    0.5 * s.total_size_y() - 0.001);
    QCHECK(std::isfinite(far));
    QCHECK_NEAR(far, edge, 1e-9);
    QCHECK(std::isfinite(f.normal_at(1e6, -1e6).x));
}

QTEST(terrain_normal_points_up) {
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


QTEST(terrain_body_support) {
    TerrainSpec s;
    s.subterrain_types = {SubterrainType::Flat};
    s.horizontal_scale = 0.5;
    const Heightfield field(s);
    TerrainBody tb(field);

    QCHECK_NEAR(tb.support_height(1.0, 2.0), 0.0, 1e-12);
    QCHECK_NEAR(tb.place_sphere(1.0, 2.0, 0.5), 0.5, 1e-12);

    QCHECK_NEAR(tb.support_force(0.0, 5.0, 0.0, 0.5).length(), 0.0, 1e-12);
    const Vec3 f = tb.support_force(0.0, 0.2, 0.0, 0.5);
    QCHECK(f.y > 0.0);
    const Vec3 f2 = tb.support_force(0.0, -1.0, 0.0, 0.5);
    QCHECK(f2.y > f.y);
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
    const Vec3 f = tb.support_force(0.0, -0.3, 0.0, 0.2);
    QCHECK(std::fabs(f.x) > 1e-6);
    QCHECK(f.y > 0.0);
}