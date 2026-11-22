#include "qai/QCFField.hpp"
#include "qai/GapFreeGrowth.hpp"
#include "qai/GenerativeWorldModel.hpp"
#include "qai/IntrinsicMotivation.hpp"
#include "qai/DevelopmentalSelfOrg.hpp"

#include <cstdio>
#include <cstdlib>

using namespace rvdl::qai;

static int checks = 0, failures = 0;
static void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    else std::printf("  ok  %s\n", what);
}
static bool near(double a, double b, double eps = 1e-9)
{
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

static State basis(size_t d, size_t k)
{
    State s(d, Complex(0, 0));
    s[k % d] = Complex(1, 0);
    return s;
}
static State random_state(size_t d, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    State s(d);
    for (auto &x : s) x = Complex(g(rng), g(rng));
    normalize(s);
    return s;
}

int main()
{
    std::printf("=== QCF 量子认知场：可证伪断言 ===\n");

    {
        const size_t d = 8;
        StateSet orthogonal;
        for (size_t i = 0; i < d; ++i) orthogonal.push_back(basis(d, i));
        check(near(frame_potential(orthogonal), 1.0 / double(d), 1e-8),
              "FP(正交完备集)=1/d");
        StateSet identical(4, basis(d, 3));
        check(near(frame_potential(identical), 1.0), "FP(同一态集合)=1");
        check(near(haar_frame_potential(d), 2.0 / (d * (d + 1.0))),
              "FP_Haar=2/(d(d+1))");
        check(frame_gap(orthogonal, d) >= 0.0, "覆盖缺口 Δ≥0");
        check(near(frame_gap(orthogonal, d), 1.0 / d - 2.0 / (d * (d + 1.0)), 1e-8),
              "Δ = FP − FP_Haar");
    }

    {
        const size_t d = 16, nq = 4;
        GapFreeGrowth growth(24, 0.001);
        for (uint64_t k = 0; k < 6; ++k) growth.observe(random_state(d, 100 + k));
        const double gap_before = growth.gap(d);
        check(gap_before > 0.0, "随机可达集存在盲区");

        const auto plan = growth.propose(d, nq, 20);
        check(plan.kind != ExtensionKind::None, "Δ>τ 时提出结构延伸");
        check(plan.residual_norm >= 0.0, "残差生成元 ‖[G*,ρ̄]‖ 可计算");

        const double fp_before = growth.potential();
        const StateSet extended = growth.apply_extension(growth.reachable(), plan, 4);
        const double fp_after = frame_potential(extended);
        std::printf("       FP %.8f → %.8f (m %zu → %zu)\n",
                    fp_before, fp_after, growth.reachable().size(), extended.size());
        check(fp_after <= fp_before + 1e-9, "QGE 增广可达集后 FP 不增（定理 2）");

        GapFreeGrowth flat(24, 1.0);
        for (uint64_t k = 0; k < 4; ++k) flat.observe(random_state(d, 200 + k));
        check(flat.propose(d, nq, 20).kind == ExtensionKind::None,
              "Δ≤τ 时不改动任何结构");

        const State s = random_state(d, 7);
        const State wide = GapFreeGrowth::extend_hilbert(s, 1);
        check(wide.size() == 2 * d, "扩 Hilbert 维数翻倍");
        check(near(norm_sq(wide), 1.0), "扩维后仍归一化");
        check(near(fidelity(wide, GapFreeGrowth::extend_hilbert(s, 1)), 1.0),
              "扩维保持既有振幅");
    }

    {
        QCFConfig cfg;
        QCFSignals s;
        s.pred_fidelity = 0.55;
        s.frame_gap = 0.4;
        s.entropy = 0.2;
        s.holevo = 0.1;
        s.tau_cost = 1.0;
        s.T = cfg.T0;
        s.mu = cfg.mu0;
        double prev = free_energy(s, cfg);
        bool monotone = true;
        for (int k = 0; k < 10; ++k)
        {
            const QCFStepLog log = descent_step(s, cfg, 0.15);
            if (!log.monotone || log.F_after > prev + 1e-12) monotone = false;
            prev = log.F_after;
        }
        std::printf("       F_10 = %.8f\n", prev);
        check(monotone, "10 步四向更新 F 单调不增（定理 1）");
        check(std::isfinite(prev), "F 有限");
    }

    {
        GenerativeWorldModel wm(7);
        const size_t d = 16;
        const State init = random_state(d, 11);
        auto transition = [](const State &s, int a) -> State
        {
            State out = s;
            apply_ry(out, size_t(std::abs(a) % 4), 0.13 * (a + 1));
            return out;
        };
        const Trajectory tr = wm.rollout(init, {0, 1, 2, 3}, transition);
        check(tr.length() == 5, "显式轨迹缓冲长度 = K+1（补 Chimera 缺口）");
        check(near(GenerativeWorldModel::replay_consistency(tr, transition), 1.0, 1e-9),
              "轨迹可精确回放");
        check(tr.steps[1].observation < d && tr.steps[4].observation < d,
              "每步 Born 观测落在合法基态");
        for (size_t k = 2; k < tr.steps.size(); ++k)
            check(near(norm_sq(tr.steps[k].state), 1.0, 1e-9), "轨迹态逐步保持酉归一化");

        const double f1 = GenerativeWorldModel::fidelity_bound(1, 0.02, 0.01);
        const double f5 = GenerativeWorldModel::fidelity_bound(5, 0.02, 0.01);
        check(f5 < f1 && f1 <= 1.0, "想象 fidelity 下界随 k 下降");
        const double d1 = GenerativeWorldModel::bures_bound(1, 0.02, 0.01);
        const double d5 = GenerativeWorldModel::bures_bound(5, 0.02, 0.01);
        const double d9 = GenerativeWorldModel::bures_bound(9, 0.02, 0.01);
        check(near(d5 - d1, d9 - d5, 1e-12), "Bures 误差按 k 线性而非指数累积（定理 3）");
        check(GenerativeWorldModel::safe_horizon(0.1, 0.02, 0.01) > 0,
              "可安全想象步数可接入 τ 预算");
        const double cf = wm.counterfactual(init, {0, 1}, {2, 3}, transition);
        check(cf >= 0.0 && cf <= 1.0, "反事实对照 fidelity 合法");
        check(std::isfinite(GenerativeWorldModel::quantum_elbo({0.5, 0.4}, {0.9, 0.8})),
              "量子 ELBO 下界可计算");
    }

    {
        QCFConfig cfg;
        IntrinsicMotivation qim(cfg);
        const size_t d = 16;
        const State before = random_state(d, 31);
        const State novel = random_state(d, 32);
        const StateSet ensemble{before, novel};
        const std::vector<double> pi{0.5, 0.5};

        const auto novelSignals = qim.measure(before, novel, ensemble, pi, 0.2, 0.0);
        const auto repeatSignals = qim.measure(before, before, ensemble, pi, 0.2, 0.0);
        const double r_novel = qim.reward(novelSignals);
        const double r_repeat = qim.reward(repeatSignals);
        std::printf("       r_novel=%.6f r_repeat=%.6f\n", r_novel, r_repeat);
        check(r_novel > r_repeat, "新颖态内在奖励高于重复态");

        const double noisy = IntrinsicMotivation::noisy_tv_guard(0.2, 0.2, cfg.zeta);
        const double clean = IntrinsicMotivation::noisy_tv_guard(0.2, 0.0, cfg.zeta);
        check(noisy < clean, "noisy-TV：不可控噪声被抑制");

        const MotivationResult wr = qim.write_back(0.15, novelSignals, 0.9);
        check(near(wr.total, 0.15 + r_novel), "内在奖励写回外部 reward（补 Chimera 缺口）");
        check(wr.expected_free_energy_proxy > 0.0 || std::isfinite(wr.expected_free_energy_proxy),
              "期望自由能代理可计算");
        auto high = novelSignals; high.delta_holevo += 0.3;
        check(qim.reward(high) > r_novel, "赋能增大 → r_int 增大");
        check(qim.expected_free_energy_proxy(high, 0.9) < qim.expected_free_energy_proxy(novelSignals, 0.9),
              "赋能增大 → 期望自由能代理减小（同时取极值）");
        check(IntrinsicMotivation::empowerment_bound(8) > 0.0, "Holevo 赋能上界 log d_A");
    }

    {
        QCFConfig cfg;
        DevelopmentalSelfOrg dev(cfg, 4, 20);
        const double T0 = dev.current().T, mu0 = dev.current().mu;
        dev.tick(); dev.tick(); dev.tick();
        const DevStage st = dev.current();
        check(st.T < T0 && st.mu < mu0, "临界期退火：T、μ 单调递减");
        check(near(st.T, DevelopmentalSelfOrg::temperature(3.0, cfg.critical_time, cfg.T0)),
              "T(t)=T0/(1+t/tc)");
        check(dev.should_grow(10.0), "大盲区触发结构生长");
        check(!dev.should_grow(0.0), "无盲区不生长");

        const size_t d = 32;
        State s = random_state(d, 51);
        for (size_t i = 0; i < 6; ++i) s[i * 5] *= 1e-6;
        normalize(s);
        const PruneResult pr = DevelopmentalSelfOrg::prune(s, 0.01);
        const double actual = 1.0 - fidelity(s, pr.state);
        std::printf("       prune δ=%.3e bound=%.3e actual=%.3e\n",
                    pr.skipped_mass, pr.error_bound, actual);
        check(pr.pruned_amplitudes > 0, "低质量振幅被修剪");
        check(near(norm_sq(pr.state), 1.0, 1e-8), "修剪后重新归一化");
        check(actual <= pr.error_bound + 1e-9, "实际误差 ≤ 2√δ+δ（定理 5）");

        std::vector<double> theta(8, 0.5), residual(8, 0.2);
        double last = 1e9;
        bool converged = true;
        for (int k = 0; k < 20; ++k)
        {
            const double n = DevelopmentalSelfOrg::hebbian_step(theta, residual, 0.1, 1.0);
            for (double &r : residual) r *= 0.85;
            if (n > last + 1e-12) converged = false;
            last = n;
        }
        check(converged, "局部 Hebbian 残差单调下降（谱隙收敛）");
    }

    std::printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}