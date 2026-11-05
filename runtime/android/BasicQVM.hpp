#pragma once
// ============================================================================
// BasicQVM.hpp —— 零依赖纯 C++ 态矢量量子后端（CPU / 内存）
//
// 用途：
//   1. Android 轻量运行时（runtime_android.cpp 的默认后端）；
//   2. 桌面无 Kokkos 环境的推理自检（qlm_android.cpp 的 selftest）；
//   3. Flutter FFI 路径的默认后端。
//
// 只实现 IQuantumBackend 的 9 个纯虚 + 非破坏期望；其余门由接口默认组合。
// 索引约定：qubit id ↔ 状态索引的位 id（id 位 = 1 ↔ |1>）；新增 qubit 为最高位、
// 初始 |0>。Rz(θ)=diag(1,e^{iθ})（与 QVM/IdealStateCore 同约定）。
//
// 与上游（桌面 daemon）的关系：门语义等价，因此训练（Transmitter/train_cli，
// 走 Kokkos QVM）与推理（本后端）在同一输入分布上工作。
// ============================================================================
#include "qhal/IQuantumBackend.hpp"

#include <complex>
#include <random>
#include <vector>

namespace qlm_cpu
{
class BasicQVM : public qhal::IQuantumBackend
{
private:
    std::vector<std::complex<double>> sv{std::complex<double>(1.0, 0.0)};
    size_t n = 0;
    std::vector<char> allocated; // 位占用表：1 = 已分配
    std::mt19937_64 rng{0x517cc1b7};

    size_t dim() const { return sv.size(); }

    void ensure(size_t id)
    {
        while (n <= id) grow();
        if (allocated.size() < n) allocated.resize(n, 0);
    }

    // 新增一位（最高位），初始 |0>：new[i] = sv[i]，new[i + 2^n] = 0
    void grow()
    {
        const size_t old = dim();
        std::vector<std::complex<double>> next(old * 2);
        for (size_t i = 0; i < old; ++i) next[i] = sv[i];
        sv = std::move(next);
        n += 1;
    }

    // 把位 id 投影回 |0>（复用已释放 id 时清掉残留纠缠分量）
    void project_zero(size_t id)
    {
        const size_t mask = size_t(1) << id;
        const size_t d = dim();
        for (size_t i = 0; i < d; ++i)
            if (i & mask) sv[i] = 0.0;
        normalize();
    }

    void normalize()
    {
        double sum = 0.0;
        for (const auto &a : sv) sum += std::norm(a);
        const double inv = sum > 0.0 ? 1.0 / std::sqrt(sum) : 1.0;
        for (auto &a : sv) a *= inv;
    }

public:
    void allocate_qubits(size_t num_qubits) override
    {
        while (n < num_qubits) grow();
        if (allocated.size() < n) allocated.resize(n, 0);
    }
    void allocate_qubit(size_t qubit_id) override
    {
        ensure(qubit_id);
        if (allocated[qubit_id]) project_zero(qubit_id);
        allocated[qubit_id] = 1;
    }
    void release_qubit(size_t qubit_id) override
    {
        if (qubit_id < allocated.size()) allocated[qubit_id] = 0;
    }
    void lock_hardware_id(size_t) override {}
    void unlock_hardware_id(size_t) override {}

    int measure(size_t qubit_id) override
    {
        ensure(qubit_id);
        const size_t mask = size_t(1) << qubit_id;
        double p1 = 0.0;
        for (size_t i = 0; i < dim(); ++i)
            if (i & mask) p1 += std::norm(sv[i]);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const int result = u(rng) < p1 ? 1 : 0;
        for (size_t i = 0; i < dim(); ++i)
            if (((i >> qubit_id) & 1u) != static_cast<unsigned>(result)) sv[i] = 0.0;
        normalize();
        return result;
    }

    double expectation_z(size_t qubit_id) override
    {
        ensure(qubit_id);
        const size_t mask = size_t(1) << qubit_id;
        double p1 = 0.0;
        for (size_t i = 0; i < dim(); ++i)
            if (i & mask) p1 += std::norm(sv[i]);
        return 1.0 - 2.0 * p1;
    }

    void apply_h(size_t qubit_id) override
    {
        ensure(qubit_id);
        const size_t mask = size_t(1) << qubit_id;
        const double s = std::sqrt(0.5);
        for (size_t i = 0; i < dim(); ++i)
        {
            if (i & mask) continue; // 每组只处理低位半侧
            auto a = sv[i];
            auto b = sv[i | mask];
            sv[i] = s * (a + b);
            sv[i | mask] = s * (a - b);
        }
    }

    void apply_x(size_t qubit_id) override
    {
        ensure(qubit_id);
        const size_t mask = size_t(1) << qubit_id;
        for (size_t i = 0; i < dim(); ++i)
        {
            if (i & mask) continue;
            std::swap(sv[i], sv[i | mask]);
        }
    }

    void apply_rz(size_t qubit_id, double angle) override
    {
        ensure(qubit_id);
        const size_t mask = size_t(1) << qubit_id;
        const std::complex<double> phase(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < dim(); ++i)
            if (i & mask) sv[i] *= phase;
    }

    void apply_cnot(size_t control, size_t target) override
    {
        ensure(control > target ? control : target);
        const size_t cmask = size_t(1) << control;
        const size_t tmask = size_t(1) << target;
        for (size_t i = 0; i < dim(); ++i)
        {
            if (!(i & cmask) || (i & tmask)) continue;
            std::swap(sv[i], sv[i ^ tmask]);
        }
    }

    void apply_toffoli(size_t c1, size_t c2, size_t target) override
    {
        ensure(max_of(c1, c2, target));
        const size_t m1 = size_t(1) << c1;
        const size_t m2 = size_t(1) << c2;
        const size_t tm = size_t(1) << target;
        for (size_t i = 0; i < dim(); ++i)
        {
            if (!(i & m1) || !(i & m2) || (i & tm)) continue;
            std::swap(sv[i], sv[i ^ tm]);
        }
    }

private:
    static size_t max_of(size_t a, size_t b, size_t c)
    {
        return a > b ? (a > c ? a : c) : (b > c ? b : c);
    }
};
} // namespace qlm_cpu