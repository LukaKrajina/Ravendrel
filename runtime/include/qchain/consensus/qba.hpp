#pragma once
//
// qchain :: 共识层 —— 量子拜占庭(Byzantine)一致（QDBA）
//
// 基于 GHZ 纠缠态的「可检测拜占庭一致」：n 个参与方共享 n-qubit GHZ 态
//   |GHZ_n> = (|0…0> + |1…1>) / √2
// 每一方以随机基（Z / X）测量自己的量子比特。GHZ 态的关联约束：
//   1) Z 基测量者的结果必须完全一致；
//   2) X 基测量者结果的异或必须为 0（模 2）。
// 恶意（拜占庭）方篡改其报告会破坏上述约束，从而被诚实方检测出来。
//
#include "../common.hpp"
#include "../../qhal/IQuantumBackend.hpp"
#include <set>
#include <vector>

namespace qchain::consensus
{

    using qchain::Byte;
    using qchain::Rng;

    // GHZ 态制备：|0…0> + |1…1> / √2（H(0) + CNOT(0, i)）。
    inline void create_ghz(qhal::IQuantumBackend *backend, size_t n)
    {
        backend->allocate_qubits(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (backend->measure(i) == 1)
                backend->apply_x(i); // 复位到 |0>
        }
        backend->apply_h(0);
        for (size_t i = 1; i < n; ++i)
            backend->apply_cnot(0, i);
    }

    struct QbaResult
    {
        bool agreement = false;       // 是否达成一致（无拜占庭干扰）
        size_t detected_rounds = 0;   // 检测到不一致的轮数
        size_t total_rounds = 0;
        std::vector<int> outcomes;    // 最后一轮的各方结果
    };

    class QuantumByzantineAgreement
    {
    public:
        //
        // 运行 QDBA。
        //   backend     : 量子后端
        //   n_parties   : 参与方数量
        //   corrupt     : 拜占庭（恶意）方索引集合
        //   num_rounds  : 重复轮数（多次抽样以稳健检测）
        //   seed        : 随机种子（0 表示使用系统熵）
        //
        static QbaResult run(qhal::IQuantumBackend *backend, size_t n_parties,
                             const std::set<size_t> &corrupt, size_t num_rounds,
                             uint64_t seed = 0)
        {
            Rng rng(seed);
            QbaResult res;
            res.total_rounds = num_rounds;

            for (size_t round = 0; round < num_rounds; ++round)
            {
                create_ghz(backend, n_parties);
                std::vector<int> out(n_parties);

                // 每轮全体使用同一全局基（Z 或 X），保证 GHZ 关联约束有确定形式。
                bool global_x = rng.random_bit();
                for (size_t i = 0; i < n_parties; ++i)
                {
                    if (global_x)
                        backend->apply_h(i);
                    out[i] = backend->measure(i);
                }

                // 拜占庭方篡改其经典报告（翻转）。
                for (size_t i : corrupt)
                    out[i] ^= 1;

                // 关联约束检查：
                //   Z 基 → 各方结果必须完全一致；
                //   X 基 → 各方结果异或必须为 0。
                bool consistent = true;
                if (global_x)
                {
                    int x_xor = 0;
                    for (int v : out)
                        x_xor ^= v;
                    consistent = (x_xor == 0);
                }
                else
                {
                    int ref = out.empty() ? 0 : out[0];
                    for (int v : out)
                        if (v != ref)
                            consistent = false;
                }

                if (!consistent)
                    ++res.detected_rounds;

                // 记录最后一轮。
                if (round + 1 == num_rounds)
                    res.outcomes = out;

                // 释放 GHZ 量子比特。
                for (size_t i = 0; i < n_parties; ++i)
                    backend->release_qubit(i);
            }

            res.agreement = (res.detected_rounds == 0);
            return res;
        }
    };
}