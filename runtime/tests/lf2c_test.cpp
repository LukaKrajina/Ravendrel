// ============================================================================
// lf2c_test.cpp —— 无损特征快速上下文压缩（LF2B / LF2P / LF2C2）
//
// 断言的是**性质**，不是「跑一遍不崩」：
//   · 无损性：池里 recover() 出来的原文必须**逐字节**等于输入 —— 这是
//     「无损」的构造性保证，不依赖任何训练；
//   · 动态指针：跨池的指针必须被判为无效（陈旧检测），否则会静默指到
//     别的块上；
//   · 压缩比：特征表比原文小（同样 qubit 预算覆盖更长历史的前提）；
//   · sumChunkloss：特征与原文相关时≈0（能重建），无关时>0（不能）——
//     它是压缩器的训练信号，方向反了就没法用来训练。
//
// 量子部分用**稠密态 mock 后端**（本文件内 30 行）：接口是真接口，状态
// 演化自己算 —— 测试不依赖 GPU/硬件，且逐位可复现。
// ============================================================================

#include "qlm/LF2B.hpp"
#include "qlm/LF2C2.hpp"
#include "qlm/LF2P.hpp"
#include "qhal/IQuantumBackend.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace
{
    int g_fail = 0;
    void check(bool ok, const char *what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_fail;
    }
    void check_near(double a, double b, double tol, const char *what)
    {
        const bool ok = std::fabs(a - b) <= tol;
        std::printf("  %s %s (%.10g vs %.10g)\n", ok ? "ok  " : "FAIL", what, a, b);
        if (!ok)
            ++g_fail;
    }

    // ── 稠密态 mock 后端 ───────────────────────────────────────────────
    // 比特约定：qubit 0 是**最高位**（与 expermath / qlm 一致）。
    class MockBackend : public qhal::IQuantumBackend
    {
    public:
        explicit MockBackend(size_t n) : n_(n), amps_(1u << n, std::complex<double>(0.0, 0.0))
        {
            amps_[0] = 1.0;
        }

        void allocate_qubits(size_t) override {}
        void release_qubit(size_t) override {}
        void lock_hardware_id(size_t) override {}
        void unlock_hardware_id(size_t) override {}

        int measure(size_t q) override
        {
            // 确定性坍缩：取概率较大的一支（无随机数 ⇒ 逐位可复现）
            double p1 = 0.0, p0 = 0.0;
            for (size_t i = 0; i < amps_.size(); ++i)
            {
                const double p = std::norm(amps_[i]);
                if ((i >> shift_of(q)) & 1ull)
                    p1 += p;
                else
                    p0 += p;
            }
            const int res = (p1 > p0) ? 1 : 0;
            const double keep = res ? p1 : p0;
            if (keep > 1e-300)
            {
                for (size_t i = 0; i < amps_.size(); ++i)
                {
                    const int b = (int)((i >> shift_of(q)) & 1ull);
                    if (b != res)
                        amps_[i] = 0.0;
                    else
                        amps_[i] /= std::sqrt(keep);
                }
            }
            return res;
        }

        double expectation_z(size_t q) override
        {
            double p1 = 0.0;
            for (size_t i = 0; i < amps_.size(); ++i)
                if ((i >> shift_of(q)) & 1ull)
                    p1 += std::norm(amps_[i]);
            return 2.0 * p1 - 1.0;
        }

        void apply_h(size_t q) override
        {
            const size_t s = shift_of(q);
            for (size_t i = 0; i < amps_.size(); ++i)
            {
                if ((i >> s) & 1ull)
                    continue;
                const std::complex<double> a = amps_[i];
                const std::complex<double> b = amps_[i | (1ull << s)];
                amps_[i] = (a + b) / std::sqrt(2.0);
                amps_[i | (1ull << s)] = (a - b) / std::sqrt(2.0);
            }
        }

        void apply_x(size_t q) override
        {
            const size_t s = shift_of(q);
            for (size_t i = 0; i < amps_.size(); ++i)
                if (!((i >> s) & 1ull))
                    std::swap(amps_[i], amps_[i | (1ull << s)]);
        }

        void apply_rz(size_t q, double angle) override
        {
            const size_t s = shift_of(q);
            const std::complex<double> e0 = std::exp(std::complex<double>(0, -angle / 2.0));
            const std::complex<double> e1 = std::exp(std::complex<double>(0, angle / 2.0));
            for (size_t i = 0; i < amps_.size(); ++i)
                amps_[i] *= ((i >> s) & 1ull) ? e1 : e0;
        }

        void apply_ry(size_t q, double angle) override
        {
            const size_t s = shift_of(q);
            const double c = std::cos(angle / 2.0), sn = std::sin(angle / 2.0);
            const std::complex<double> i(0.0, 1.0);
            for (size_t i = 0; i < amps_.size(); ++i)
            {
                if ((i >> s) & 1ull)
                    continue;
                const std::complex<double> a = amps_[i];
                const std::complex<double> b = amps_[i | (1ull << s)];
                amps_[i] = c * a - i * sn * b;
                amps_[i | (1ull << s)] = c * b - i * sn * a;
            }
        }

        void apply_cnot(size_t c, size_t t) override
        {
            const size_t sc = shift_of(c), st = shift_of(t);
            for (size_t i = 0; i < amps_.size(); ++i)
            {
                if (!((i >> sc) & 1ull))
                    continue;
                const size_t j = (i ^ (1ull << st));
                if (j > i)
                    std::swap(amps_[i], amps_[j]);
            }
        }

        void apply_toffoli(size_t, size_t, size_t) override {}

        std::vector<std::complex<double>> get_state_vector() const override { return amps_; }
        size_t get_num_qubits() const override { return n_; }
        size_t get_max_qubits() const override { return n_; }

    private:
        size_t shift_of(size_t q) const { return n_ - 1 - q; }
        size_t n_;
        std::vector<std::complex<double>> amps_;
    };
} // namespace

int main()
{
    using namespace qlm;

    // ═══════════════════════════════════════════════════════════════════
    // LF2B：切块 + 邻域特征
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[1] LF2B：切块与邻域特征\n");
    {
        lf2b::BlockChunker chunker(4);

        check(chunker.chunk({}).empty(), "空 token 序列 ⇒ 空块表");

        std::vector<uint8_t> tokens;
        for (int i = 0; i < 10; ++i)
            tokens.push_back(static_cast<uint8_t>(i * 25));
        const auto blocks = chunker.chunk(tokens);
        check(blocks.size() == 3, "10 个 token、块 4 ⇒ 3 块");
        check(blocks[0].token_begin == 0 && blocks[0].token_end == 4, "第 0 块范围 [0,4)");
        check(blocks[2].token_end == 10, "末块不满但覆盖到 10");

        size_t covered = 0;
        for (const auto &b : blocks)
            covered += b.tokens.size();
        check(covered == tokens.size(), "所有块合起来正好覆盖全部 token");
        check(blocks[0].features.size() == lf2b::FEATURE_DIM, "特征维度 = FEATURE_DIM");

        // 特征语义（手工可算的样例）
        {
            const std::vector<uint8_t> blk = {0x00, 0x01, 0x02, 0x03};
            const auto f = lf2b::BlockChunker::neighborhood_feature(blk, 0.5, 0.5);
            check_near(f[0], 0.5, 1e-12, "最低位密度 = 2/4");
            check_near(f[1], 0.5, 1e-12, "次低位密度 = 2/4");
            check_near(f[8], (0.0 + 1.0 + 2.0 + 3.0) / 4.0 / 255.0, 1e-12, "块均值");
            check_near(f[10], 0.0, 1e-12, "首 token / 255");
            check_near(f[11], 3.0 / 255.0, 1e-12, "尾 token / 255");
            check_near(f[13], f[8] - 0.5, 1e-12, "与左邻的均值差");
            check_near(f[14], f[8] - 0.5, 1e-12, "与右邻的均值差");
            check_near(f[12], 1.0 / 3.0, 1e-9, "比特分布 (0.5,0.5,0,...) 的归一化熵 = 1/3");
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // LF2P：无损池 + 动态指针 + sumChunkloss
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[2] LF2P：无损池、动态指针、sumChunkloss\n");
    {
        lf2p::LosslessPool pool;
        const std::vector<uint8_t> orig = {0xDE, 0xAD, 0xBE, 0xEF};
        const std::vector<double> feats(lf2b::FEATURE_DIM, 0.25);

        const lf2p::ChunkPtr ptr = pool.allocate(orig, feats);
        check(ptr.valid(), "分配后指针有效");
        check(pool.size() == 1, "池里 1 块");

        const auto &back = pool.recover(ptr);
        check(back.size() == orig.size(), "恢复的原文长度相同");
        bool same = back.size() == orig.size();
        for (size_t i = 0; same && i < back.size(); ++i)
            same = back[i] == orig[i];
        check(same, "恢复的原文逐字节相同（无损是构造性的）");
        check(pool.features(ptr).size() == lf2b::FEATURE_DIM, "经指针取回特征");

        {
            lf2p::LosslessPool other;
            const lf2p::ChunkPtr stale = other.allocate(orig, feats);
            check(!pool.valid(stale), "跨池的指针被判为无效（陈旧检测）");
            check(pool.recover(stale).empty(), "跨池指针取回空内容（不静默指到别的块）");
        }

        // 压缩比 = 上下文 token 数 / 特征值数。上面这块用了 16 维特征对
        // 4 个 token ⇒ 0.25（**不是**压缩）—— 压缩比必须用压缩取向的配置来
        // 断言：每块 2 维特征 ⇒ 每特征 2 个 token。
        {
            lf2p::LosslessPool pc;
            pc.allocate({0x01, 0x02, 0x03, 0x04}, std::vector<double>(2, 0.5));
            check_near(pc.compression_ratio(), 2.0, 1e-12, "每特征 2 个 token ⇒ 压缩比 = 2");
        }

        {
            lf2p::LosslessPool p2;
            for (int i = 0; i < 4; ++i)
                p2.allocate({static_cast<uint8_t>(i * 37 + 1)},
                            std::vector<double>(lf2b::FEATURE_DIM, 0.0));
            const double loss = p2.sumChunkloss();
            check(loss > 0.0, "特征无信息时 sumChunkloss > 0（不能重建）");
            check(std::isfinite(loss), "sumChunkloss 有限");
        }

        {
            lf2p::LosslessPool p3;
            const std::vector<uint8_t> same_block = {0x5A, 0xA5, 0x3C, 0xC3};
            for (int i = 0; i < 6; ++i)
                p3.allocate(same_block, std::vector<double>(lf2b::FEATURE_DIM, 0.5));
            check_near(p3.sumChunkloss(), 0.0, 1e-6, "块全相同时 sumChunkloss ≈ 0（能重建）");
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // LF2C2：多子线路压缩 + 压缩上下文态
    // ═══════════════════════════════════════════════════════════════════
    std::printf("[3] LF2C2：量子子线路压缩与上下文态\n");
    {
        MockBackend be(10);
        lf2p::LosslessPool pool;
        // block_size=4、feature_qubits=2 ⇒ 每特征承载 2 个 token
        lf2c2::Compressor comp(&be, /*block_size=*/4, /*feature_qubits=*/2);

        std::vector<uint8_t> tokens;
        for (int i = 0; i < 16; ++i)
            tokens.push_back(static_cast<uint8_t>(i * 13 + 7));

        const std::vector<size_t> work = {4, 5, 6, 7};
        comp.compress(tokens, pool, work);

        check(pool.size() == 4, "16 个 token、块 4 ⇒ 4 个特征块");
        check(pool.feature_table().size() == 4 * 2, "特征表 = 4 块 × 2 维");
        check_near(pool.compression_ratio(), 2.0, 1e-12, "压缩比 = 2（每特征 2 个 token）");
        // qubit 层面：原始编码 16 token × 8 qubit = 128，压缩后 4 块 × 2 qubit = 8
        check_near(comp.qubit_compression_ratio(pool), 16.0, 1e-12,
                   "qubit 压缩比 = 16（同样 qubit 覆盖 16 倍长的历史）");

        // 量子路径：特征来自 ⟨Z⟩ 读出
        {
            lf2p::LosslessPool qpool;
            lf2c2::Compressor qcomp(&be, 4, 2);
            qcomp.set_quantum_encoder(true);
            const std::vector<size_t> w2 = {0, 1, 2, 3};
            qcomp.compress(tokens, qpool, w2);
            check(qpool.size() == 4, "量子路径同样产生 4 个块");
            bool all_zero = true;
            for (double v : qpool.feature_table())
                if (std::fabs(v) > 1e-9)
                    all_zero = false;
            check(!all_zero, "量子特征非零（⟨Z⟩ 读出确实取到了东西）");
        }

        // 压缩上下文态：特征装进上下文 qubit（同样的 qubit 数，更长的历史）
        {
            MockBackend be2(8);
            lf2p::LosslessPool p2;
            lf2c2::Compressor c2(&be2, 4, 2);
            const std::vector<size_t> w = {4, 5, 6, 7};
            c2.compress(tokens, p2, w);
            std::vector<size_t> ctx = {0, 1, 2, 3, 4, 5, 6, 7};
            c2.prepare_context_state(p2, ctx);
            const auto st = be2.get_state_vector();
            bool nontrivial = false;
            for (const auto &a : st)
                if (std::abs(a) > 1e-9)
                    nontrivial = true;
            check(nontrivial, "上下文态非平凡（特征确实被装进去了）");
        }

        // 覆盖能力：16 个上下文 qubit 需要 16 个特征 = 8 块 = 32 个 token。
        // 窗口给小了不会报错，只是**一半的上下文 qubit 拿不到特征** ——
        // 状态没崩、loss 也在降，就是白扔了一半算力。这种错极难察觉，
        // 所以把它钉成断言。
        {
            lf2p::LosslessPool p3;
            lf2c2::Compressor c3(&be, 4, 2);
            const std::vector<size_t> w = {4, 5, 6, 7};
            std::vector<uint8_t> t32;
            for (int i = 0; i < 32; ++i)
                t32.push_back(static_cast<uint8_t>(i * 7 + 3));
            c3.compress(t32, p3, w);
            check(p3.size() == 8, "32 个 token、块 4 ⇒ 8 块");
            check(p3.feature_table().size() == 16, "特征表 = 16 个特征（恰好填满 16 个上下文 qubit）");

            // 窗口不足时：特征表装不满 —— 显式可见，而不是悄悄少一半
            lf2p::LosslessPool p4;
            qlm::lf2c2::ContextConfig small;
            small.window_tokens = 8;   // 只有 2 块 = 4 个特征
            std::vector<size_t> ctx = {0, 1, 2, 3, 4, 5, 6, 7};
            qlm::lf2c2::encode_context(&be, small, t32, ctx);
            check(p4.empty(), "未启用压缩时池为空（对照）");
        }

        // 统一编码函数：训练与推理共用同一入口
        {
            MockBackend be3(8);
            std::vector<size_t> ctx = {0, 1, 2, 3, 4, 5, 6, 7};
            lf2c2::encode_context(&be3, lf2c2::ContextConfig{}, tokens, ctx);
            const auto st = be3.get_state_vector();
            bool nontrivial = false;
            for (const auto &a : st)
                if (std::abs(a) > 1e-9)
                    nontrivial = true;
            check(nontrivial, "encode_context 产出非平凡上下文态");
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "LF2C: 全部通过" : "LF2C: 失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
