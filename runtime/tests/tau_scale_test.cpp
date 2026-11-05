// tau_scale_test.cpp —— 量子时间缩微调度（QτS）的性质测试
//
// 断言对象：runtime/include/qml/TauScale.hpp 的数学性质。全部用预先手算的
// 答案，不做随机对照（浮点断言带 1e-12 容差）：
//   1. 分层复合 compose：单位权重 = 各层之和；加权形式的手算值
//   2. 代际法则 generational / anneal：τ/α；α ≤ 0 返原值
//   3. 领域缩放因子 alpha_of / alpha_preset：移动 1.3 / 自驾 1.5 / AI 10 / 未知 1
//   4. 折叠深度 fold_depth：⌈L/T⌉ 及 T 的边界（0 → L、T ≥ L → 1）
//   5. 齿轮比 gear_ratio：比值与零守卫（全跨层 → 2^53 大数）
//   6. 调度代价 route_cost：τ_gate·D + τ_comm·D·g 的手算值
//   7. 关键路径 critical_path：链 / 菱形 / 空图 / 环（不悬挂、有限值）
//   8. 影子价格 route_price：v − λ·c
//   9. 时间预算路由 budget_route：全装得下 → 全选 λ=0；预算可行性与
//      单调性（预算变大不丢已选项）；λ ∈ [0, max_ratio+1]
//
// 与 pb_test / moe_test 一样只依赖头文件 + STL，不链 rvdl_rt。

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
    // ── 1. 分层复合 ──────────────────────────────────────────────────────
    {
        TauLayers t{1.0, 2.0, 3.0, 4.0};
        TauWeights w{1.0, 1.0, 1.0, 1.0};
        CHECK_NEAR(compose(t, w), 10.0); // 单位权重 = 各层和

        TauWeights w2{1.0, 2.0, 1.5, 3.0};
        // 1·1 + 2·2 + 1.5·3 + 3·4 = 1 + 4 + 4.5 + 12 = 21.5
        CHECK_NEAR(compose(t, w2), 21.5);

        TauLayers zero{};
        CHECK_NEAR(compose(zero, w2), 0.0);
    }

    // ── 2. 代际法则 τ_{n+1} = τ_n / α ────────────────────────────────────
    {
        CHECK_NEAR(generational(10.0, 2.5), 4.0);
        CHECK_NEAR(generational(1.0, 10.0), 0.1);
        CHECK_NEAR(generational(5.0, 0.0), 5.0);   // α ≤ 0 返原值
        CHECK_NEAR(generational(5.0, -2.0), 5.0);
        CHECK_NEAR(anneal(10.0, 2.0), 5.0);        // 退火与代际同公式
    }

    // ── 3. 领域缩放因子（论文第 2 节：1.3 / 1.5 / 10）────────────────────
    {
        CHECK_NEAR(alpha_of(AlphaDomain::Mobile), 1.3);
        CHECK_NEAR(alpha_of(AlphaDomain::Autonomous), 1.5);
        CHECK_NEAR(alpha_of(AlphaDomain::Ai), 10.0);
        CHECK_NEAR(alpha_preset(0), 1.3);
        CHECK_NEAR(alpha_preset(1), 1.5);
        CHECK_NEAR(alpha_preset(2), 10.0);
        CHECK_NEAR(alpha_preset(7), 1.0);  // 未知预设：不缩放
        CHECK_NEAR(alpha_preset(-1), 1.0);
    }

    // ── 4. 折叠深度 ⌈L/T⌉（LogicFolding 量子化）──────────────────────────
    {
        CHECK_NEAR(fold_depth(12.0, 4.0), 3.0);
        CHECK_NEAR(fold_depth(12.0, 5.0), 3.0);  // ceil(12/5)=ceil(2.4)=3
        CHECK_NEAR(fold_depth(12.0, 1.0), 12.0); // 不折叠
        CHECK_NEAR(fold_depth(7.0, 7.0), 1.0);   // T ≥ L → 1
        CHECK_NEAR(fold_depth(5.0, 10.0), 1.0);
        CHECK_NEAR(fold_depth(5.0, 0.0), 5.0);   // T ≤ 0 视为未折叠
        CHECK_NEAR(fold_depth(5.0, -1.0), 5.0);
    }

    // ── 5. 齿轮比 g = E_cross / E_intra ──────────────────────────────────
    {
        CHECK_NEAR(gear_ratio(6.0, 2.0), 3.0);   // 论文阈值 3
        CHECK_NEAR(gear_ratio(1.0, 3.0), 1.0 / 3.0);
        CHECK_NEAR(gear_ratio(0.0, 0.0), 0.0);   // 无通信
        CHECK_NEAR(gear_ratio(5.0, 0.0), 9.007199254740992e15); // 全是跨层通信
        CHECK_NEAR(gear_ratio(0.0, 4.0), 0.0);   // 无跨层边
    }

    // ── 6. 调度代价 τ_route = τ_gate·D + τ_comm·D·g ──────────────────────
    {
        CHECK_NEAR(route_cost(10.0, 0.0, 1.0, 0.1), 10.0);  // 无跨层通信
        CHECK_NEAR(route_cost(10.0, 3.0, 1.0, 0.1), 13.0);  // 10 + 10·3·0.1
        CHECK_NEAR(route_cost(0.0, 3.0, 1.0, 0.1), 0.0);    // 零深度
        CHECK_NEAR(route_cost(-2.0, 3.0, 1.0, 0.1), 0.0);   // 负深度按 0
    }

    // ── 7. 关键路径（DAG 最长依赖链）──────────────────────────────────────
    {
        // 链 0→1→2，代价 {1,2,3}：1+2+3 = 6
        std::vector<double> c{1.0, 2.0, 3.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e{{0, 1}, {1, 2}};
        CHECK_NEAR(critical_path(c, e), 6.0);

        // 菱形 0→1、0→2、1→3、2→3，代价全 1：路径 0-1-3 = 3
        std::vector<double> c2{1.0, 1.0, 1.0, 1.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e2{{0, 1}, {0, 2}, {1, 3}, {2, 3}};
        CHECK_NEAR(critical_path(c2, e2), 3.0);

        // 空图
        std::vector<double> c3;
        std::vector<std::pair<std::int32_t, std::int32_t>> e3;
        CHECK_NEAR(critical_path(c3, e3), 0.0);

        // 环 0→1→0（非法输入）：必须有限返回，不能悬挂
        std::vector<double> c4{1.0, 1.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e4{{0, 1}, {1, 0}};
        volatile double cyc = critical_path(c4, e4);
        CHECK(cyc >= 0.0); // 只保证有限且非负

        // 非法边（越界/自环）忽略：0→0 自环不贡献
        std::vector<double> c5{2.0, 5.0};
        std::vector<std::pair<std::int32_t, std::int32_t>> e5{{0, 0}, {0, 2}, {7, 1}};
        CHECK_NEAR(critical_path(c5, e5), 5.0); // max(2, 5)
    }

    // ── 8. 时间影子价格 v − λ·c ──────────────────────────────────────────
    {
        CHECK_NEAR(route_price(5.0, 2.0, 1.0), 3.0);
        CHECK_NEAR(route_price(5.0, 2.0, 5.0), -5.0);
        CHECK_NEAR(route_price(5.0, 2.0, 0.0), 5.0); // 时间为 0 价 → 只余价值
    }

    // ── 9. 时间预算路由（拉格朗日对偶）────────────────────────────────────
    {
        // 全装得下 → λ=0 全选
        {
            std::vector<double> v{1.0, 2.0}, c{1.0, 1.0};
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 10.0, chosen);
            CHECK_NEAR(lam, 0.0);
            CHECK_NEAR(val, 3.0);
            CHECK_NEAR(cost, 2.0);
            CHECK(chosen.size() == 2);
        }
        // 空输入
        {
            std::vector<double> v, c;
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 1.0, chosen);
            CHECK_NEAR(lam, 0.0);
            CHECK_NEAR(val, 0.0);
            CHECK_NEAR(cost, 0.0);
            CHECK(chosen.empty());
        }
        // 手算小例 v=[6,5,3], t=[3,2,1]：B=4 时结果必须可行且非空
        {
            std::vector<double> v{6.0, 5.0, 3.0}, c{3.0, 2.0, 1.0};
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 4.0, chosen);
            CHECK(cost <= 4.0 + 1e-12); // 可行性
            CHECK(!chosen.empty());     // B=4 至少装得下最便宜的一项
            CHECK(val > 0.0);
            CHECK(lam >= 0.0 && lam <= 4.0); // λ ∈ [0, max_ratio+1]，max_ratio=3
        }
        // 预算小到一项都装不下 → 空集
        {
            std::vector<double> v{6.0, 5.0, 3.0}, c{3.0, 2.0, 1.0};
            std::vector<std::int32_t> chosen;
            auto [lam, val, cost] = budget_route(v, c, 0.5, chosen);
            CHECK(chosen.empty());
            CHECK_NEAR(cost, 0.0);
        }
        // 单调性：预算变大，入选集合不减（B=1 → {2}；B=4 ⊇ {2}）
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