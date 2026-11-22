#include <Kokkos_Core.hpp>

#include "qhal/QuantumAccel.hpp"
#include "qlm/QLM.hpp"
#include "qlm/AdjointDevice.hpp"
#include "qlm/BatchAdjoint.hpp"
#include "qlm/RasAdjointDevice.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Complex = std::complex<double>;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) { ++g_failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}
static bool nearly_equal(double a, double b, double eps = 1e-8)
{
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

static std::vector<Complex> make_state(size_t n)
{
    std::vector<Complex> s(n);
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double re = std::sin(0.13 * double(i) + 0.7);
        const double im = std::cos(0.07 * double(i) + 1.1);
        s[i] = Complex(re, im);
        acc += std::norm(s[i]);
    }
    const double inv = 1.0 / std::sqrt(acc);
    for (auto &x : s) x *= inv;
    return s;
}

static std::vector<double> host_expectation_z(const std::vector<Complex> &s,
                                              const std::vector<size_t> &bits)
{
    std::vector<double> out(bits.size(), 0.0);
    for (size_t j = 0; j < bits.size(); ++j)
    {
        const size_t mask = size_t(1) << bits[j];
        double p1 = 0.0;
        for (size_t i = 0; i < s.size(); ++i)
            if (i & mask) p1 += std::norm(s[i]);
        out[j] = 1.0 - 2.0 * p1;
    }
    return out;
}

int main(int argc, char **argv)
{
    Kokkos::initialize(argc, argv);
    {
        const rvdl::accel::Detection det = rvdl::accel::detect();
        const rvdl::accel::DeviceInfo sel = rvdl::accel::select(false);
        std::printf("[accel] %s\n", rvdl::accel::describe(sel).c_str());
        std::printf("[accel] compiled=%s exec=%s nvidia=%d amd=%d intel=%d\n",
                    rvdl::accel::backend_name(rvdl::accel::compiled_backend()),
                    rvdl::accel::execution_space_name(),
                    det.nvidia ? 1 : 0, det.amd ? 1 : 0, det.intel ? 1 : 0);
        check(sel.backend == rvdl::accel::compiled_backend(), "select 报告编译后端");
        if (rvdl::accel::compiled_backend() != rvdl::accel::Backend::Cpu)
            check(sel.usable == (det.nvidia || det.amd || det.intel), "探测到 GPU 时应可用或给出回退理由");
        if (det.nvidia && rvdl::accel::compiled_backend() == rvdl::accel::Backend::Cuda)
            check(sel.vendor == rvdl::accel::Vendor::Nvidia && sel.usable, "NVIDIA 应被选中并启用 CUDA");

        const size_t N = size_t(1) << 8;
        std::vector<Complex> base = make_state(N);
        std::vector<size_t> bits = {0, 1, 2, 3, 4, 5, 6, 7};

        auto verify_gate = [&](const char *label, int kind, size_t q, size_t q1, double angle)
        {
            std::vector<Complex> host = base;
            rvdl::accel::StateVectorDevice dev(N);
            dev.upload(base.data(), N);
            if (kind == 0) { qlm::train_detail::sv_apply_ry(host, q, angle); dev.apply_ry(q, angle); }
            else if (kind == 1) { qlm::train_detail::sv_apply_rz(host, q, angle); dev.apply_rz(q, angle); }
            else { qlm::train_detail::sv_apply_cnot(host, q, q1); dev.apply_cnot(q, q1); }
            std::vector<Complex> got(N);
            dev.download(got.data(), N);
            double maxerr = 0.0;
            for (size_t i = 0; i < N; ++i)
                maxerr = std::max(maxerr, std::abs(got[i] - host[i]));
            std::printf("[gate ] %s maxerr=%.3e\n", label, maxerr);
            check(maxerr < 1e-12, label);
        };
        verify_gate("RY", 0, 3, 0, 0.73);
        verify_gate("RZ", 1, 5, 0, -1.21);
        verify_gate("CNOT", 2, 2, 6, 0.0);

        std::vector<Complex> evolved = base;
        qlm::train_detail::sv_apply_ry(evolved, 3, 0.73);
        qlm::train_detail::sv_apply_cnot(evolved, 2, 6);
        qlm::train_detail::sv_apply_rz(evolved, 5, -1.21);
        const std::vector<double> ref = host_expectation_z(evolved, bits);

        rvdl::accel::StateVectorDevice dev(N);
        dev.upload(evolved.data(), N);
        const std::vector<double> by_kernel = dev.batch_expectation_z(bits);
        const std::vector<double> by_host = rvdl::accel::qm_batch_expectation_z(evolved, bits, false);
        double e_kernel = 0.0, e_host = 0.0;
        for (size_t j = 0; j < bits.size(); ++j)
        {
            e_kernel = std::max(e_kernel, std::fabs(by_kernel[j] - ref[j]));
            e_host = std::max(e_host, std::fabs(by_host[j] - ref[j]));
        }
        std::printf("[expr ] kernel_maxerr=%.3e host_maxerr=%.3e\n", e_kernel, e_host);
        check(e_kernel < 1e-8, "批量 ⟨Z⟩ kernel 与 host 参考一致");
        check(e_host < 1e-12, "批量 ⟨Z⟩ host 回退与参考一致");

        const size_t n_q = 8, layers = 2;
        const std::vector<double> lapse = {1.0, 0.9};
        const auto gates = qlm::train_detail::build_gates(n_q, layers, lapse);
        const size_t params = layers * (n_q + 8);
        std::vector<double> theta(params);
        for (size_t i = 0; i < params; ++i) theta[i] = 0.01 * double(i) + 0.003;
        const uint8_t target = 0xB7;
        const std::vector<size_t> readout = {0, 1, 2, 3, 4, 5, 6, 7};

        std::vector<double> grad_h(params, 0.0), pred_h;
        std::vector<Complex> phi_h, lam_h;
        double acc_h = 0.0;
        const double loss_h = qlm::train_detail::adjoint_loss_grad(
            gates, evolved, theta.data(), readout, target, grad_h, pred_h, phi_h, lam_h, acc_h);

        std::vector<double> grad_d(params, 0.0), pred_d;
        std::vector<Complex> phi_d, lam_d;
        double acc_d = 0.0;
        const double loss_d = rvdl::accel::device_adjoint(
            gates, evolved.data(), evolved.size(), theta.data(), readout, target,
            grad_d, pred_d, phi_d, lam_d, acc_d);

        double e_loss = std::fabs(loss_h - loss_d), e_grad = 0.0, e_pred = 0.0;
        for (size_t i = 0; i < params; ++i) e_grad = std::max(e_grad, std::fabs(grad_h[i] - grad_d[i]));
        for (size_t j = 0; j < readout.size(); ++j)
            e_pred = std::max(e_pred, std::fabs(pred_h[j] - pred_d[j]));
        std::printf("[adj  ] loss %.12f / %.12f  grad_maxerr=%.3e pred_maxerr=%.3e\n",
                    loss_h, loss_d, e_grad, e_pred);
        check(nearly_equal(loss_h, loss_d, 1e-8), "device adjoint loss parity");
        check(e_grad < 1e-7, "device adjoint 梯度 parity（GPU 归约顺序允许微小差异）");
        check(e_pred < 1e-7, "device adjoint 预测 parity");
        check(nearly_equal(acc_h, acc_d, 1e-9), "device adjoint 准确率 parity");
        (void)e_loss;
        (void)e_pred;

        {
            const size_t experts = 4;
            const auto moe_gates = qlm::train_detail::build_gates(n_q, layers, lapse, experts);
            size_t shared = 0, per_expert = 0;
            for (const auto &g : moe_gates)
            {
                if (g.expert == qlm::train_detail::SHARED_GATE) ++shared;
                if (qlm::train_detail::gate_active(g, experts, 0)) ++per_expert;
            }
            std::printf("[moe  ] gates=%zu shared=%zu active_for_expert0=%zu\n",
                        moe_gates.size(), shared, per_expert);
            check(shared == (n_q > 8 ? (n_q - 8) * layers : 0), "CNOT 骨架门按电路结构共享");
            check(per_expert < moe_gates.size(), "每个专家只激活门序列的子集");
            check(per_expert >= shared, "共享门始终激活");

            for (size_t e = 0; e < experts; ++e)
            {
                std::vector<double> grad_h(params, 0.0), grad_d(params, 0.0), pred_h, pred_d;
                std::vector<Complex> phi_h, lam_h, phi_d, lam_d;
                double acc_h = 0.0, acc_d = 0.0;
                const double lh = qlm::train_detail::adjoint_loss_grad(
                    moe_gates, evolved, theta.data(), readout, target,
                    grad_h, pred_h, phi_h, lam_h, acc_h, experts, e);
                const double ld = rvdl::accel::device_adjoint(
                    moe_gates, evolved.data(), evolved.size(), theta.data(), readout, target,
                    grad_d, pred_d, phi_d, lam_d, acc_d, experts, e);
                double eg = 0.0;
                for (size_t i = 0; i < params; ++i) eg = std::max(eg, std::fabs(grad_h[i] - grad_d[i]));
                check(nearly_equal(lh, ld, 1e-8), "MoE device/host loss parity");
                check(eg < 1e-7, "MoE device/host 梯度 parity");
                size_t leaked = 0;
                for (size_t i = 0; i < params; ++i)
                    if (i % experts != e && grad_h[i] != 0.0) ++leaked;
                check(leaked == 0, "非激活专家的参数不产生梯度");
            }
        }

        {
            const size_t B = 4;
            std::vector<Complex> kets(B * N);
            const uint8_t tgts[B] = {0xB7, 0x42, 0x11, 0xFE};
            for (size_t b = 0; b < B; ++b)
            {
                auto s = make_state(N);
                qlm::train_detail::sv_apply_ry(s, 3, 0.31 + 0.07 * b);
                qlm::train_detail::sv_apply_rz(s, 5, -0.4 + 0.11 * b);
                std::copy(s.begin(), s.end(), kets.begin() + b * N);
            }

            std::vector<double> ref_grad(params, 0.0);
            std::vector<double> ref_loss(B, 0.0), ref_acc(B, 0.0);
            for (size_t b = 0; b < B; ++b)
            {
                const std::vector<Complex> one(kets.begin() + b * N, kets.begin() + (b + 1) * N);
                std::vector<double> g(params, 0.0), p;
                std::vector<Complex> ph, lm;
                double a = 0.0;
                ref_loss[b] = qlm::train_detail::adjoint_loss_grad(
                    gates, one, theta.data(), readout, tgts[b], g, p, ph, lm, a);
                ref_acc[b] = a;
                for (size_t i = 0; i < params; ++i) ref_grad[i] += g[i] / double(B);
            }
            double ref_loss_avg = 0.0, ref_acc_avg = 0.0;
            for (size_t b = 0; b < B; ++b) { ref_loss_avg += ref_loss[b] / double(B); ref_acc_avg += ref_acc[b] / double(B); }

            for (size_t tb : {size_t(0), size_t(4)})
            {
                const auto r = rvdl::accel::device_adjoint_batch(
                    gates, kets.data(), B, N, theta.data(), readout, tgts,
                    params, tb, 0.0);
                double eg = 0.0;
                for (size_t i = 0; i < params; ++i) eg = std::max(eg, std::fabs(r.grad[i] - ref_grad[i]));
                double el = 0.0;
                for (size_t b = 0; b < B; ++b) el = std::max(el, std::fabs(r.sample_loss[b] - ref_loss[b]));
                std::printf("[bat  ] tile=%zu grad_maxerr=%.3e loss_maxerr=%.3e eps=%.1e\n",
                            tb, eg, el, r.error_bound);
                check(nearly_equal(r.loss, ref_loss_avg, 1e-8), "BAT 平均损失 parity");
                check(eg < 1e-7, "BAT 批量梯度 parity（tile 只改归约顺序）");
                check(el < 1e-7, "BAT 逐样本损失 parity");
                check(nearly_equal(r.accuracy, ref_acc_avg, 1e-8), "BAT 准确率 parity");
                check(r.error_bound == 0.0, "eps=0 时误差上界必须为 0（精确）");
                check(r.active_tiles == r.total_tiles, "eps=0 时所有 tile 活跃");
            }

            {
                const auto r = rvdl::accel::device_adjoint_batch(
                    gates, kets.data(), B, N, theta.data(), readout, tgts,
                    params, 6, 0.06);
                double err = 0.0;
                for (size_t i = 0; i < params; ++i) err = std::max(err, std::fabs(r.grad[i] - ref_grad[i]));
                std::printf("[ebat ] skipped_mass=%.4e bound=%.4e actual_grad_err=%.4e tiles=%zu/%zu\n",
                            r.skipped_mass, r.error_bound, err, r.active_tiles, r.total_tiles);
                check(r.skipped_mass > 0.0 && r.active_tiles < r.total_tiles, "EBAT 确实裁剪了 tile");
                check(r.error_bound > 0.0, "EBAT 报告可计算误差上界");
                check(err <= r.error_bound + 1e-12, "实际梯度误差不超过理论上界");
            }
        }

        std::vector<rvdl::accel::Gate> plan_input;
        rvdl::accel::Gate rz1; rz1.type = rvdl::accel::Gate::Type::RZ; rz1.q0 = 2; rz1.param = 5; rz1.scale = 1.0;
        rvdl::accel::Gate rz2 = rz1;
        rvdl::accel::Gate zero; zero.type = rvdl::accel::Gate::Type::RY; zero.q0 = 1; zero.param = 3; zero.scale = 0.0;
        rvdl::accel::Gate cx; cx.type = rvdl::accel::Gate::Type::CNOT; cx.q0 = 0; cx.q1 = 7;
        plan_input = {rz1, rz2, zero, cx};
        const rvdl::accel::CircuitPlan plan = rvdl::accel::fuse_gates(plan_input);
        std::printf("[fuse ] %zu → %zu  fused=%zu eliminated=%zu\n",
                    plan.original_count, plan.gates.size(), plan.fused_count, plan.eliminated);
        check(plan.eliminated == 1, "零角旋转被消除");
        check(plan.fused_count >= 1, "相邻同参相位门被合并");
        check(plan.gates.size() < plan.original_count, "融合后门数减少");

        const auto shots_dev = rvdl::accel::qm_sample_shots(evolved, 256, 12345, true);
        const auto shots_host = rvdl::accel::qm_sample_shots(evolved, 256, 12345, false);
        check(shots_dev.size() == 256 && shots_host.size() == 256, "shot 数量");
        bool in_range = true;
        for (size_t s : shots_dev) if (s >= N) in_range = false;
        for (size_t s : shots_host) if (s >= N) in_range = false;
        check(in_range, "shot 落在合法基态范围");
        check(shots_dev == shots_host, "device 与 host 采样同一 seed 结果一致");

        {
            const size_t qubits[] = {8, 10};
            const size_t layers[] = {1, 2};
            const size_t batches[] = {1, 3};
            const size_t tilebits[] = {0, 4};
            const double epsilons[] = {0.0, 0.02};
            const uint64_t seeds[] = {42, 777};
            for (size_t nq : qubits)
            for (size_t nl : layers)
            for (size_t B : batches)
            for (size_t tb : tilebits)
            for (double eps : epsilons)
            for (uint64_t seed : seeds)
            {
                using GateOp = qlm::train_detail::GateOp;
                const size_t NN = size_t(1) << nq, D = 8, MM = 1u << D;
                const std::vector<size_t> rbits = {nq - 8, nq - 7, nq - 6, nq - 5,
                                                  nq - 4, nq - 3, nq - 2, nq - 1};
                std::vector<GateOp> gs;
                size_t pi = 0;
                for (size_t l = 0; l < nl; ++l)
                {
                    for (size_t q = 0; q < nq; ++q)
                    { GateOp g; g.type = GateOp::Type::RY; g.q0 = q; g.q1 = 0;
                      g.param = pi++; g.scale = 0.7 + 0.1 * double(l); gs.push_back(g); }
                    for (size_t q = 0; q < nq; ++q)
                    { GateOp g; g.type = GateOp::Type::RZ; g.q0 = q; g.q1 = 0;
                      g.param = pi++; g.scale = 0.3 + 0.05 * double(l); gs.push_back(g); }
                    for (size_t q = 0; q + 1 < nq; ++q)
                    { GateOp g; g.type = GateOp::Type::CNOT; g.q0 = q; g.q1 = q + 1;
                      g.param = 0; g.scale = 1.0; gs.push_back(g); }
                }
                const size_t params = pi;
                std::vector<double> th(params);
                for (size_t i = 0; i < params; ++i)
                    th[i] = 0.13 * std::sin(double(i) + double(seed));

                std::vector<Complex> ket(B * NN);
                for (size_t b = 0; b < B; ++b)
                {
                    const auto one = make_state(NN);
                    for (size_t i = 0; i < NN; ++i) ket[b * NN + i] = one[i];
                }
                std::vector<uint8_t> tg(B);
                for (size_t b = 0; b < B; ++b) tg[b] = uint8_t(0x37 + 0x2B * b);

                const auto dense = rvdl::accel::device_adjoint_batch(
                    gs, ket.data(), B, NN, th.data(), rbits, tg.data(), params, 0, 0.0);

                std::vector<unsigned char> gt(gs.size());
                std::vector<size_t> q0(gs.size()), q1(gs.size()), prm(gs.size());
                std::vector<double> sc(gs.size());
                for (size_t g = 0; g < gs.size(); ++g)
                {
                    gt[g] = static_cast<unsigned char>(
                        gs[g].type == GateOp::Type::RY ? 0 :
                        gs[g].type == GateOp::Type::RZ ? 1 : 2);
                    q0[g] = gs[g].q0; q1[g] = gs[g].q1;
                    prm[g] = gs[g].param; sc[g] = gs[g].scale;
                }
                std::vector<double> ket_pod(B * NN * 2);
                for (size_t i = 0; i < B * NN; ++i)
                { ket_pod[2 * i] = ket[i].real(); ket_pod[2 * i + 1] = ket[i].imag(); }

                qlm::AdjointRuntimeConfig cfg;
                cfg.tile_bits = tb;
                cfg.sparse_tile_bits = tb ? tb : 6;
                cfg.sparse_epsilon = eps;
                double skipped = 0.0, bound = 0.0;
                size_t at = 0, tt = 0;
                cfg.skipped_mass = &skipped; cfg.error_bound = &bound;
                cfg.active_tiles = &at; cfg.total_tiles = &tt;
                std::vector<double> sgd(params, 0.0), sl(B, 0.0), sa(B, 0.0);
                rvdl::accel::device_adjoint_sparse_batch_pod(
                    gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gs.size(),
                    ket_pod.data(), B, NN, th.data(), rbits.data(), D, tg.data(),
                    sgd.data(), params, sl.data(), sa.data(), &cfg);

                double eg = 0.0;
                for (size_t i = 0; i < params; ++i)
                    eg = std::max(eg, std::fabs(sgd[i] - dense.grad[i]));
                if (eps == 0.0)
                {
                    if (eg > 1e-9)
                    {
                        std::fprintf(stderr,
                                     "[sparse] PARITY FAIL n=%zu L=%zu B=%zu tb=%zu "
                                     "seed=%llu eg=%.3e bound=%.3e tiles=%zu/%zu\n",
                                     nq, nl, B, tb, (unsigned long long)seed,
                                     eg, bound, at, tt);
                    }
                    check(eg < 1e-9, "稀疏 exact 分支与稠密 BAT parity");
                    check(bound == 0.0 && at == tt, "稀疏 exact 分支全 tile 活跃且误差界为零");
                }
                else
                {
                    if (eg > bound + 1e-6)
                        std::fprintf(stderr,
                                     "[sparse] BOUND FAIL n=%zu L=%zu B=%zu tb=%zu "
                                     "eg=%.3e bound=%.3e\n", nq, nl, B, tb, eg, bound);
                    check(eg <= bound + 1e-6, "稀疏近似梯度误差落在报告上界内");
                    check(at <= tt && tt > 0, "活跃 tile 数合法");
                }
                (void)MM;
            }
        }

        {
            const size_t n = 10, NN = size_t(1) << n, t = 6, D = 8;
            std::vector<size_t> bits;
            for (size_t j = 0; j < D; ++j) bits.push_back(n - D + j);

            std::vector<Complex> product(NN, Complex(1.0, 0.0));
            for (size_t q = 0; q < n; ++q)
            {
                const double a = 0.11 * double(q + 1);
                const size_t m = size_t(1) << q;
                const double c = std::cos(a / 2), sn = std::sin(a / 2);
                for (size_t i = 0; i < NN; ++i)
                {
                    if (i & m) continue;
                    const Complex x = product[i], y = product[i | m];
                    product[i] = c * x - sn * y;
                    product[i | m] = sn * x + c * y;
                }
            }

            std::vector<unsigned char> gt;
            std::vector<size_t> q0, q1, prm;
            std::vector<double> sc;
            size_t P = 0;
            for (size_t q = 0; q < n; ++q)
            { gt.push_back(0); q0.push_back(q); q1.push_back(0); prm.push_back(P++); sc.push_back(0.8); }
            for (size_t q = 0; q < n; ++q)
            { gt.push_back(1); q0.push_back(q); q1.push_back(0); prm.push_back(P++); sc.push_back(0.5); }
            for (size_t q = 0; q + 1 < n; ++q)
            {
                if (q + 1 == t) continue;
                gt.push_back(2); q0.push_back(q); q1.push_back(q + 1);
                prm.push_back(0); sc.push_back(1.0);
            }
            std::vector<double> theta(P);
            for (size_t i = 0; i < P; ++i) theta[i] = 0.17 * std::sin(double(i));

            std::vector<double> pod(NN * 2);
            for (size_t i = 0; i < NN; ++i)
            { pod[2 * i] = product[i].real(); pod[2 * i + 1] = product[i].imag(); }
            const uint8_t target = 0x5A;

            qlm::AdjointRuntimeConfig cfg;
            cfg.sparse_tile_bits = t;
            cfg.sparse_epsilon = 0.0;
            cfg.ras_epsilon = 1e-2;
            cfg.ras_rank = 6;
            cfg.ras_seed = 42;
            cfg.moe_experts = 1;
            double host_bound = 0.0, dev_bound = 0.0;
            size_t host_rank = 0, dev_rank = 0, host_lam = 0, dev_lam = 0;
            cfg.skipped_mass = &host_bound; cfg.error_bound = &host_bound;
            cfg.active_tiles = &host_rank; cfg.total_tiles = &host_lam;

            std::vector<double> gh(P, 0.0), ph(D, 0.0), ah(1, 0.0);
            rvdl::ras_adjoint::ras_adjoint_batch_pod(
                gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gt.size(),
                pod.data(), 1, NN, theta.data(), bits.data(), D, &target,
                gh.data(), P, ph.data(), ah.data(), &cfg);

            qlm::AdjointRuntimeConfig dcfg = cfg;
            dcfg.skipped_mass = &dev_bound; dcfg.error_bound = &dev_bound;
            dcfg.active_tiles = &dev_rank; dcfg.total_tiles = &dev_lam;
            std::vector<double> gd(P, 0.0), pd(D, 0.0), ad(1, 0.0);
            rvdl::ras_device::ras_device_batch_pod(
                gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gt.size(),
                pod.data(), 1, NN, theta.data(), bits.data(), D, &target,
                gd.data(), P, pd.data(), ad.data(), &dcfg);

            double eg = 0.0;
            for (size_t i = 0; i < P; ++i) eg = std::max(eg, std::fabs(gh[i] - gd[i]));
            double ep = 0.0;
            for (size_t j = 0; j < D; ++j) ep = std::max(ep, std::fabs(ph[j] - pd[j]));
            std::printf("[rasdev] grad_max_err=%.3e pred_max_err=%.3e "
                        "loss=%.6f/%.6f rank=%zu/%zu lam=%zu/%zu bound=%.3e/%.3e\n",
                        eg, ep, ah[0], ad[0], host_rank, dev_rank,
                        host_lam, dev_lam, host_bound, dev_bound);
            check(std::isfinite(ad[0]) && std::isfinite(eg), "RAS device 输出有限");
            check(ep < 1e-6, "RAS device 读出与 host parity");
            check(eg < 1e-6, "RAS device 梯度与 host parity");
            check(dev_rank > 0 && dev_rank <= size_t(cfg.ras_rank) * 2 + 2,
                  "RAS device 秩上界合法");
            check(dev_lam >= dev_rank && dev_lam <= dev_rank * 2,
                  "RAS device λ 为 2r 项");

            qlm::AdjointRuntimeConfig ec = cfg;
            ec.ras_epsilon = 0.0; ec.sparse_epsilon = 0.0;
            std::vector<double> eh(P, 0.0), ed(P, 0.0), ph2(D, 0.0), pd2(D, 0.0);
            double ae1 = 0.0, ae2 = 0.0;
            rvdl::ras_adjoint::ras_adjoint_batch_pod(
                gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gt.size(),
                pod.data(), 1, NN, theta.data(), bits.data(), D, &target,
                eh.data(), P, ph2.data(), &ae1, &ec);
            rvdl::ras_device::ras_device_batch_pod(
                gt.data(), q0.data(), q1.data(), prm.data(), sc.data(), gt.size(),
                pod.data(), 1, NN, theta.data(), bits.data(), D, &target,
                ed.data(), P, pd2.data(), &ae2, &ec);
            double ee = 0.0;
            for (size_t i = 0; i < P; ++i) ee = std::max(ee, std::fabs(eh[i] - ed[i]));
            check(ee <= 1e-12, "RAS device exact 分支逐位 parity");
        }

        std::printf("[done ] %d checks, %d failures\n", g_checks, g_failures);
    }
    Kokkos::finalize();
    return g_failures == 0 ? 0 : 1;
}