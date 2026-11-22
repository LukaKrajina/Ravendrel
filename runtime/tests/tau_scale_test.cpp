#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "qml/TauScale.hpp"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                                  \
    do                                                                               \
    {                                                                                \
        g_checks++;                                                                  \
        if (!(cond))                                                                 \
        {                                                                            \
            g_failures++;                                                            \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b)                                                             \
    do                                                                               \
    {                                                                                \
        g_checks++;                                                                  \
        double va = (a), vb = (b);                                                   \
        if (!(std::fabs(va - vb) <= 1e-12))                                          \
        {                                                                            \
            g_failures++;                                                            \
            std::printf("FAIL %s:%d  %s=%g vs %s=%g\n", __FILE__, __LINE__, #a, va,  \
                        #b, vb);                                                     \
        }                                                                            \
    } while (0)

using namespace numqk::tau;

int main()
{
    {
        TauLayers t{1.0, 2.0, 3.0, 4.0};
        TauWeights w{1.0, 1.0, 1.0, 1.0};
        CHECK_NEAR(compose(t, w), 10.0);

        TauWeights w2{1.0, 2.0, 1.5, 3.0};
        CHECK_NEAR(compose(t, w2), 21.5);

        TauLayers zero{};
        CHECK_NEAR(compose(zero, w2), 0.0);
    }

    {
        CHECK_NEAR(generational(10.0, 2.5), 4.0);
        CHECK_NEAR(generational(1.0, 10.0), 0.1);
        CHECK_NEAR(generational(5.0, 0.0), 5.0);
        CHECK_NEAR(generational(5.0, -2.0), 5.0);
        CHECK_NEAR(anneal(10.0, 2.0), 5.0);
    }

    {
        CHECK_NEAR(alpha_of(AlphaDomain::Mobile), 1.3);
        CHECK_NEAR(alpha_of(AlphaDomain::Autonomous), 1.5);
        CHECK_NEAR(alpha_of(AlphaDomain::Ai), 10.0);
        CHECK_NEAR(alpha_preset(0), 1.3);
        CHECK_NEAR(alpha_preset(1), 1.5);
        CHECK_NEAR(alpha_preset(2), 10.0);
        CHECK_NEAR(alpha_preset(7), 1.0);
        CHECK_NEAR(alpha_preset(-1), 1.0);
    }

    {
        CHECK_NEAR(fold_depth(12.0, 4.0), 3.0);
        CHECK_NEAR(fold_depth(12.0, 5.0), 3.0);
        CHECK_NEAR(fold_depth(12.0, 1.0), 12.0);
        CHECK_NEAR(fold_depth(7.0, 7.0), 1.0);
        CHECK_NEAR(fold_depth(5.0, 10.0), 1.0);
        CHECK_NEAR(fold_depth(5.0, 0.0), 5.0);
        CHECK_NEAR(fold_depth(5.0, -1.0), 5.0);
    }

    {
        CHECK_NEAR(gear_ratio(6.0, 2.0), 3.0);
        CHECK_NEAR(gear_ratio(1.0, 3.0), 1.0 / 3.0);
        CHECK_NEAR(gear_ratio(0.0, 0.0), 0.0);
        CHECK_NEAR(gear_ratio(5.0, 0.0), 9.007199254740992e15);
        CHECK_NEAR(gear_ratio(0.0, 4.0), 0.0);
    }

    {
        CHECK_NEAR(route_cost(10.0, 0.0, 1.0, 0.1), 10.0);
        CHECK_NEAR(route_cost(10.0, 3.0, 1.0, 0.1), 13.0);
        CHECK_NEAR(route_cost(0.0, 3.0, 1.0, 0.1), 0.0);
        CHECK_NEAR(route_cost(-2.0, 3.0, 1.0, 0.1), 0.0);
    }

    {
        std::vector<double> c{1.0, 2.0, 3.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e{{0, 1}, {1, 2}};
        CHECK_NEAR(critical_path(c, e), 6.0);

        std::vector<double> c2{1.0, 1.0, 1.0, 1.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e2{{0, 1}, {0, 2}, {1, 3}, {2, 3}};
        CHECK_NEAR(critical_path(c2, e2), 3.0);

        std::vector<double> c3;
        std::vector<std::pair<std::int32_t, std::int32_t>> e3;
        CHECK_NEAR(critical_path(c3, e3), 0.0);

        std::vector<double> c4{1.0, 1.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e4{{0, 1}, {1, 0}};
        volatile double cyc = critical_path(c4, e4);
        CHECK(cyc >= 0.0);

        std::vector<double> c5{2.0, 5.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e5{{0, 0}, {0, 2}, {7, 1}};
        CHECK_NEAR(critical_path(c5, e5), 5.0);
    }

    {
        CHECK_NEAR(route_price(5.0, 2.0, 1.0), 3.0);
        CHECK_NEAR(route_price(5.0, 2.0, 5.0), -5.0);
        CHECK_NEAR(route_price(5.0, 2.0, 0.0), 5.0);
    }

    {
        {
            std::vector<double> v{1.0, 2.0}, c{1.0, 1.0};
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 10.0, chosen);
            CHECK_NEAR(lam, 0.0);
            CHECK_NEAR(val, 3.0);
            CHECK_NEAR(cost, 2.0);
            CHECK(chosen.size() == 2);
        }
        {
            std::vector<double> v, c;
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 1.0, chosen);
            CHECK_NEAR(lam, 0.0);
            CHECK_NEAR(val, 0.0);
            CHECK_NEAR(cost, 0.0);
            CHECK(chosen.empty());
        }
        {
            std::vector<double> v{6.0, 5.0, 3.0}, c{3.0, 2.0, 1.0};
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 4.0, chosen);
            CHECK(cost <= 4.0 + 1e-12);
            CHECK(!chosen.empty());
            CHECK(val > 0.0);
            CHECK(lam >= 0.0 && lam <= 4.0);
        }
        {
            std::vector<double> v{6.0, 5.0, 3.0}, c{3.0, 2.0, 1.0};
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 0.5, chosen);
            CHECK(chosen.empty());
            CHECK_NEAR(cost, 0.0);
        }
        {
            std::vector<double> v{6.0, 5.0, 3.0}, c{3.0, 2.0, 1.0};
            std::vector<std::int32_t> smallSel, bigSel;
            budget_route(v, c, 1.0, smallSel);
            budget_route(v, c, 4.0, bigSel);
            for (std::int32_t i : smallSel)
            {
                bool found = false;
                for (std::int32_t j : bigSel) if (j == i) { found = true; break; }
                CHECK(found);
            }
        }
    }

    std::printf("tau_scale_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}