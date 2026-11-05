#pragma once
//
// PhotonicBackend —— 光子量子计算后端（连续变量 / 高斯态范式）
//
// 与超导（微波）、离子阱（QCCD）、中性原子（里德堡）等「离散量子比特」范式不同，
// 光子量子计算以「连续变量（CV）」为核心：信息编码在光场的正交分量 (x̂, p̂) 上，
// 量子态为高斯态（真空 / 相干态 / 压缩态），门为高斯门（位移 / 压缩 / 相移 / 分束器），
// 测量为高斯测量（homodyne）与光子数测量（PNR）。
//
//
// 本后端在实现标准 IQuantumBackend（离散门）之外，用特别的办法暴露光子特有的
// 连续变量操作与噪声模型：
//   • 高斯门：displace / squeeze / phase_shift / beam_splitter
//   • 高斯测量：homodyne（正交分量）/ photon_count（PNR）
//   • 光子噪声：传输损耗 / 探测器效率 / 暗计数 / 有限压缩度
//
#include "IQuantumBackend.hpp"
#include <cstddef>
#include <cmath>
#include <random>
#include <vector>
#include <map>
#include <utility>
#include <mutex>
#include <stdexcept>
#include <iostream>
#include <complex>

namespace qhal
{

    // ─── 高斯态模式（单光子模式 = 一个量子谐振子）────────────────────
    //
    // 除均值和协方差外，升级为「完整梳态」（高斯包络叠加）：GKP 编码的逻辑态是
    // 无限个高斯峰的叠加（comb / 梳状），逻辑信息编码在峰的奇偶性上：
    //   |0⟩_L : 峰在偶 √π 处（x = 2√π·m）
    //   |1⟩_L : 峰在奇 √π 处（x = (2m+1)√π）
    //   |+⟩_L : 峰在 p 方向（Walsh-Hadamard 变换后）
    // 单模式梳态用本结构的 comb 表示；多模式纠缠（GHZ / Bell）用
    // PhotonicBackend::gkp_joint 联合梳态（峰索引的笛卡尔积）。
    struct GaussianMode
    {
        // 梳态峰：index 为 √π 的倍数，re/im 为复权重。
        struct Peak
        {
            int index = 0;
            double re = 0.0;
            double im = 0.0;
        };
        std::vector<Peak> comb;  // 完整梳态（非空 = GKP 态）
        double comb_sigma = 0.15; // 每峰高斯宽度（有限压缩 → 测量噪声）

        double x_mean = 0.0;   // ⟨x̂⟩
        double p_mean = 0.0;   // ⟨p̂⟩
        double x_var = 0.5;    // ⟨(Δx̂)²⟩，真空涨落 = 1/2
        double p_var = 0.5;
        double xp_cov = 0.0;   // 交叉协方差
        int photon_number = 0; // 离散光子数（单光子近似 0/1）
        // 单光子编码的 2 维复幅度：态 = amp0·|0⟩ + amp1·|1⟩。
        // 用于在单光子路径下正确实现 H / X / Rz 与 Born 规则坍缩。
        std::complex<double> amp0 = {1.0, 0.0};
        std::complex<double> amp1 = {0.0, 0.0};
        bool allocated = false;
        bool locked = false;
    };

    // ─── 光子噪声配置 ─────────────────────────────────────────────────
    struct PhotonicNoiseConfig
    {
        double loss_rate = 0.01;          // 每门光子损耗率
        double detector_efficiency = 0.9; // 探测器效率
        double dark_count_rate = 1e-3;    // 暗计数率
        double squeezing_db = 10.0;       // 压缩度（dB，有限压缩 → 非理想高斯态）
    };

    class PhotonicBackend : public IQuantumBackend
    {
    private:
        std::mutex optics_mutex;
        std::vector<GaussianMode> modes;
        PhotonicNoiseConfig noise;
        std::mt19937_64 rng;
        bool use_gkp = true; // 启用 GKP 编码（精确离散门，玻色子纠错范式）

        // ─── GKP 联合梳态（多模式纠缠，如 GHZ）───────────────────────
        //
        // 完整梳态的多模式联合表示为「峰的笛卡尔积」：每个峰携带 n 个模式的
        // √π 倍数索引 + 复权重。GKP 逻辑门全部退化为峰索引的仿射/相位变换。
        struct JointPeak
        {
            std::vector<int> idx; // n 个模式的 √π 倍数（峰位置）
            double re = 0.0;
            double im = 0.0;
        };
        std::vector<JointPeak> gkp_joint; // GKP 联合梳态
        size_t gkp_n = 0;                 // GKP 编码的模式数
        double gkp_sigma = 0.15;          // 每峰高斯宽度（有限压缩）

        // GKP 梳态峰索引范围：[-COMB, COMB)，共 2·COMB 个峰。
        // 取偶数个峰（8 个），使 Walsh-Hadamard 变换中 Σ_k (-1)^{k·S}(S 奇)=0
        // 精确抵消，从而 GHZ 的 X 基关联（异或=0）无截断误差。
        static constexpr int GKP_COMB = 4;

        // ─── 噪声采样 ───────────────────────────────────────────────
        double gauss_noise()
        {
            std::normal_distribution<double> dist(0.0, 1.0);
            return dist(rng);
        }
        bool bernoulli(double p)
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(rng) < p;
        }
        double uniform_unit()
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(rng);
        }

        void ensure_mode(size_t m)
        {
            if (m >= modes.size())
                modes.resize(m + 1);
        }

        // 光子损耗：门操作后，光子以 loss_rate 概率丢失。
        void apply_loss(size_t m)
        {
            if (modes[m].photon_number == 1 && bernoulli(noise.loss_rate))
                modes[m].photon_number = 0;
        }

        // 相移（高斯门）：x,p 旋转 φ。
        void phase_shift(size_t m, double phi)
        {
            double c = std::cos(phi), s = std::sin(phi);
            double x = modes[m].x_mean, p = modes[m].p_mean;
            modes[m].x_mean = c * x - s * p;
            modes[m].p_mean = s * x + c * p;
            apply_loss(m);
        }

    public:
        PhotonicBackend() { rng.seed(std::random_device{}()); }
        explicit PhotonicBackend(const PhotonicNoiseConfig &cfg) : noise(cfg) { rng.seed(std::random_device{}()); }

        const PhotonicNoiseConfig &get_noise() const { return noise; }

        // 切换离散门编码：false = 单光子（光子数）编码；true = GKP 编码（精确高斯门）。
        void set_gkp_encoding(bool enable) { use_gkp = enable; }
        bool get_gkp_encoding() const { return use_gkp; }

        // ─── IQuantumBackend 基础接口 ────────────────────────────────
        void allocate_qubits(size_t num_qubits) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (num_qubits > modes.size())
                modes.resize(num_qubits);
            for (size_t i = 0; i < num_qubits; ++i)
                modes[i].allocated = true;
            // GKP 编码：初始化联合梳态为 |0…0⟩_L（单峰，全偶 index=0）。
            if (use_gkp)
            {
                gkp_n = num_qubits;
                gkp_joint.clear();
                JointPeak p;
                p.idx.assign(num_qubits, 0);
                p.re = 1.0;
                p.im = 0.0;
                gkp_joint.push_back(std::move(p));
            }
        }
        void release_qubit(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (qubit_id < modes.size())
                modes[qubit_id].allocated = false;
        }
        void lock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < modes.size())
                modes[qubit_id].locked = true;
        }
        void unlock_hardware_id(size_t qubit_id) override
        {
            if (qubit_id < modes.size())
                modes[qubit_id].locked = false;
        }

        // ─── 光子特有：连续变量高斯门 ────────────────────────────────
        // 位移（displacement）：D(α)，α 为复数。
        void displace(size_t mode, double alpha_re, double alpha_im)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            const double sqrt2 = std::sqrt(2.0);
            modes[mode].x_mean += sqrt2 * alpha_re;
            modes[mode].p_mean += sqrt2 * alpha_im;
        }

        // 压缩（squeezing）：S(r)，r 为压缩参数（或按 dB）。
        void squeeze(size_t mode, double r)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            modes[mode].x_var *= std::exp(-2.0 * r);
            modes[mode].p_var *= std::exp(2.0 * r);
        }

        // 相移（phase shifter）：R(φ)。
        void phase_shift_gate(size_t mode, double phi)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            phase_shift(mode, phi);
        }

        // 分束器（beam splitter）：两个模式按透过率 cosθ 混合。
        void beam_splitter(size_t a, size_t b, double theta)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(a);
            ensure_mode(b);
            double c = std::cos(theta), s = std::sin(theta);
            double xa = modes[a].x_mean, pa = modes[a].p_mean;
            double xb = modes[b].x_mean, pb = modes[b].p_mean;
            modes[a].x_mean = c * xa + s * xb;
            modes[b].x_mean = -s * xa + c * xb;
            modes[a].p_mean = c * pa + s * pb;
            modes[b].p_mean = -s * pa + c * pb;
            apply_loss(a);
            apply_loss(b);
        }

        // ─── 光子特有：高斯测量 ──────────────────────────────────────
        // homodyne：测量正交分量 x̂ cosφ + p̂ sinφ，返回连续读数（含真空涨落噪声）。
        double homodyne(size_t mode, double phi)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            double c = std::cos(phi), s = std::sin(phi);
            double mean = c * modes[mode].x_mean + s * modes[mode].p_mean;
            double var = c * c * modes[mode].x_var + s * s * modes[mode].p_var;
            return mean + std::sqrt(var) * gauss_noise();
        }

        // 光子数测量（PNR）：n = (x² + p² − 1)/2 的期望，四舍五入，含探测器效率与暗计数。
        int photon_count(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(mode);
            double n_mean = (modes[mode].x_mean * modes[mode].x_mean +
                             modes[mode].p_mean * modes[mode].p_mean +
                             modes[mode].x_var + modes[mode].p_var - 1.0) / 2.0;
            if (n_mean < 0.0)
                n_mean = 0.0;
            // 探测器效率：真实光子以概率被探测。
            double detected = modes[mode].photon_number * (bernoulli(noise.detector_efficiency) ? 1.0 : 0.0);
            // 暗计数：以暗计数率出现假光子。
            if (bernoulli(noise.dark_count_rate))
                detected += 1.0;
            return static_cast<int>(std::round(detected + (n_mean > 0.5 ? 0.5 : 0.0)));
        }

        // ─── GKP 编码的精确离散门（完整梳态 / 玻色子纠错范式）───────
        //
        // GKP（Gottesman–Kitaev–Preskill）态把逻辑 qubit 编码到连续变量的 √π 晶格上，
        // 逻辑态是「完整梳态」（无限高斯峰的叠加），逻辑门全部退化为峰索引的
        // 仿射/相位变换（位移 / Walsh-Hadamard / 求和门）——光子/玻色子纠错的核心
        // （arXiv:2308.02913；GKP 芯片源 Nature 2025）。
        //
        //   |0⟩_L : 峰在偶 √π（x = 2√π·m）
        //   |1⟩_L : 峰在奇 √π（x = (2m+1)√π）
        //   X_L   : index → index + 1（位移 √π）
        //   Z_L   : 奇峰相位翻转（index → index，相位 ×(−1)^index）
        //   H_L   : Walsh-Hadamard 变换 c'_j = Σ_k c_k (−1)^{k·j} / √N
        //   CNOT_L: index_t += index_c（求和门）
        static constexpr double GKP_SQRT_PI = 1.772453850905516; // √π

        // 逻辑 X（所有峰 index += 1，奇偶翻转）。
        void gkp_x(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            for (auto &p : gkp_joint)
                if (mode < p.idx.size())
                    p.idx[mode] += 1;
        }

        // 逻辑 Z（奇峰相位翻转）。
        void gkp_z(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            for (auto &p : gkp_joint)
            {
                if (mode < p.idx.size() && (p.idx[mode] & 1))
                {
                    p.re = -p.re;
                    p.im = -p.im;
                }
            }
        }

        // 逻辑 H（Walsh-Hadamard 变换，完整梳态的傅里叶变换）。
        // c'_j = (1/√N) Σ_k c_k (−1)^{k·j}，k 为 mode 维度的峰索引，j ∈ [−COMB, COMB)。
        void gkp_h(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (mode >= gkp_n)
                return;
            const double norm = 1.0 / std::sqrt(2.0 * static_cast<double>(GKP_COMB));
            std::map<std::vector<int>, std::pair<double, double>> merged;
            for (const auto &p : gkp_joint)
            {
                int k = p.idx[mode];
                for (int j = -GKP_COMB; j < GKP_COMB; ++j)
                {
                    auto idx = p.idx;
                    idx[mode] = j;
                    double sign = ((k * j) & 1) ? -1.0 : 1.0;
                    auto &acc = merged[idx];
                    acc.first += p.re * sign * norm;
                    acc.second += p.im * sign * norm;
                }
            }
            gkp_joint.clear();
            gkp_joint.reserve(merged.size());
            for (auto &kv : merged)
                gkp_joint.push_back({std::move(kv.first), kv.second.first, kv.second.second});
        }

        // 逻辑 CNOT（求和门：index_t += index_c）。
        void gkp_cnot(size_t control, size_t target)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            for (auto &p : gkp_joint)
                if (target < p.idx.size() && control < p.idx.size())
                    p.idx[target] += p.idx[control];
        }

        // 逻辑测量：按 |权重|² 采样峰，读 mode 维度峰位置 + 高斯噪声（有限压缩），
        // 取模 2√π 判定逻辑 0/1。测量为「投影坍缩」：保留 idx[mode] 奇偶与结果一致
        // 的峰（归一化），从而保持多模式纠缠（GHZ/Bell）的关联。
        int gkp_measure(size_t mode)
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            if (gkp_joint.empty())
                return 0;
            double total = 0.0;
            for (const auto &p : gkp_joint)
                total += p.re * p.re + p.im * p.im;
            if (total <= 0.0)
                return 0;
            double r = uniform_unit() * total;
            double acc = 0.0;
            int sampled_index = 0;
            for (const auto &p : gkp_joint)
            {
                acc += p.re * p.re + p.im * p.im;
                if (r < acc)
                {
                    sampled_index = (mode < p.idx.size()) ? p.idx[mode] : 0;
                    break;
                }
            }
            // 峰位置 + 有限压缩噪声 → 取模 2√π，用「最近峰」判定奇偶：
            // 偶峰 mod ≈ 0（或 2√π），奇峰 mod ≈ √π，边界在 √π/2 与 3√π/2。
            double x = sampled_index * GKP_SQRT_PI + gkp_sigma * gauss_noise();
            double mod = std::fmod(x, 2.0 * GKP_SQRT_PI);
            if (mod < 0.0)
                mod += 2.0 * GKP_SQRT_PI;
            int result = (mod >= GKP_SQRT_PI / 2.0 && mod < 1.5 * GKP_SQRT_PI) ? 1 : 0;

            // 投影坍缩：只保留 idx[mode] 奇偶 == result 的峰，归一化。
            int parity = result;
            std::vector<JointPeak> collapsed;
            collapsed.reserve(gkp_joint.size());
            double new_total = 0.0;
            for (const auto &p : gkp_joint)
            {
                if (mode < p.idx.size() && ((p.idx[mode] & 1) == parity))
                {
                    collapsed.push_back(p);
                    new_total += p.re * p.re + p.im * p.im;
                }
            }
            if (new_total > 0.0)
            {
                double norm = 1.0 / std::sqrt(new_total);
                for (auto &p : collapsed)
                {
                    p.re *= norm;
                    p.im *= norm;
                }
                gkp_joint.swap(collapsed);
            }
            return result;
        }

        // ─── @[noise]/@[coherence] 噪声通道分发（光子损耗/相位扰动/比特翻转）───
        void apply_noise(size_t qubit_id, int channel, double param) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            auto &m = modes[qubit_id];
            switch (channel)
            {
                case 0: // depolarizing：以 3p/4 施加 X/Y/Z 之一（Paulí 通道）
                {
                    const double r = uniform_unit();
                    const double px = param / 4.0;
                    if (r < px)
                    {
                        // X：交换 |0⟩/|1⟩ 振幅
                        std::swap(m.amp0, m.amp1);
                    }
                    else if (r < 2 * px)
                    {
                        // Y = iXZ：交换并施加 ±i 相位
                        const std::complex<double> I(0.0, 1.0);
                        const std::complex<double> t = m.amp0;
                        m.amp0 = -I * m.amp1;
                        m.amp1 = I * t;
                    }
                    else if (r < 3 * px)
                    {
                        // Z：|1⟩ 振幅相位翻转
                        m.amp1 *= std::complex<double>(-1.0, 0.0);
                    }
                    break;
                }
                case 1: // dephasing（相位翻转）
                    if (bernoulli(param)) m.amp1 *= std::complex<double>(-1.0, 0.0);
                    break;
                case 2: // amplitude damping（光子损耗）
                    if (bernoulli(param)) m.photon_number = 0;
                    break;
                case 3: // bit flip（光子数翻转）
                    if (bernoulli(param)) m.photon_number ^= 1;
                    break;
                default: break;
            }
        }

        // ─── 离散门（单光子编码或 GKP 编码 + 噪声）─────────────────
        // X 门：单光子编码下翻转 2 维复幅度；GKP 编码下位移 √π。
        void apply_x(size_t qubit_id) override
        {
            if (use_gkp)
            {
                gkp_x(qubit_id);
                return;
            }
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            std::swap(modes[qubit_id].amp0, modes[qubit_id].amp1);
            modes[qubit_id].photon_number ^= 1;
            apply_loss(qubit_id);
        }

        // H 门：单光子编码下真实 Hadamard；GKP 编码下 Walsh-Hadamard 变换。
        void apply_h(size_t qubit_id) override
        {
            if (use_gkp)
            {
                gkp_h(qubit_id);
                return;
            }
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            // 真 Hadamard：|0⟩ -> (|0⟩+|1⟩)/√2，|1⟩ -> (|0⟩-|1⟩)/√2。
            const double inv = 1.0 / std::sqrt(2.0);
            auto a0 = modes[qubit_id].amp0;
            auto a1 = modes[qubit_id].amp1;
            modes[qubit_id].amp0 = inv * (a0 + a1);
            modes[qubit_id].amp1 = inv * (a0 - a1);
            // 连续变量侧保留 90° 相移（CV 语义），不再用它近似 Hadamard。
            phase_shift(qubit_id, M_PI / 2.0);
        }

        // Rz 门：单光子编码下 |1⟩ 分支加相位；GKP 编码下相移（通用）。
        void apply_rz(size_t qubit_id, double angle) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            modes[qubit_id].amp1 *= std::complex<double>(std::cos(angle), std::sin(angle));
            phase_shift(qubit_id, angle);
        }

        // CNOT 门：单光子编码下概率性线性光学；GKP 编码下求和门。
        void apply_cnot(size_t control, size_t target) override
        {
            if (use_gkp)
            {
                gkp_cnot(control, target);
                return;
            }
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(control);
            ensure_mode(target);
            if (modes[control].photon_number == 1)
                modes[target].photon_number ^= 1;
            apply_loss(control);
            apply_loss(target);
        }

        // Toffoli 门：双控制翻转（单光子编码）。
        void apply_toffoli(size_t c1, size_t c2, size_t target) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(c1);
            ensure_mode(c2);
            ensure_mode(target);
            if (modes[c1].photon_number == 1 && modes[c2].photon_number == 1)
                modes[target].photon_number ^= 1;
            apply_loss(c1);
            apply_loss(c2);
            apply_loss(target);
        }

        // 测量：单光子编码下按 |amp1|² Born 规则采样坍缩；GKP 编码下 homodyne 晶格判定。
        int measure(size_t qubit_id) override
        {
            if (use_gkp)
                return gkp_measure(qubit_id);
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            double p1 = std::norm(modes[qubit_id].amp1);
            int result = bernoulli(p1) ? 1 : 0;
            // 探测器效率：光子可能漏测。
            if (result == 1 && !bernoulli(noise.detector_efficiency))
                result = 0;
            // 暗计数：假光子。
            if (result == 0 && bernoulli(noise.dark_count_rate))
                result = 1;
            // Born 规则坍缩到计算基。
            modes[qubit_id].amp0 = (result == 0) ? std::complex<double>(1.0, 0.0) : std::complex<double>(0.0, 0.0);
            modes[qubit_id].amp1 = (result == 1) ? std::complex<double>(1.0, 0.0) : std::complex<double>(0.0, 0.0);
            modes[qubit_id].photon_number = result;
            return result;
        }

        // ─── 纠缠验证（E91 / CHSH，GKP 编码）─────────────────────────
        //
        // 制备 Bell 对 |Φ+⟩ = (|00⟩ + |11⟩)/√2（H(0) + CNOT(0,1)），测 Z 基与 X 基
        // 的一致率。纠缠见证 W = ⟨Z_A Z_B⟩ + ⟨X_A X_B⟩ = 2(Pz + Px) − 2：
        //   W ≤ 0  可分态（无纠缠）
        //   W > 0  存在纠缠（等价于 CHSH 不等式违背 |S| > 2）
        // 理想 |Φ+⟩：W = 2（对应 CHSH |S| = 2√2 ≈ 2.828）。
        struct BellResult
        {
            double z_agreement = 0.0; // Z 基一致率
            double x_agreement = 0.0; // X 基一致率
            double witness = 0.0;     // 纠缠见证 W（> 0 表示纠缠）
            double chsh = 0.0;        // CHSH 估计值（≈ 2√2·(W/2)，|S|>2 表示 Bell 违背）
        };

        // 制备 Bell 对（GKP 编码）：|Φ+⟩ = (|00⟩+|11⟩)/√2。
        void prepare_bell()
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            gkp_n = 2;
            gkp_joint.clear();
            JointPeak p;
            p.idx.assign(2, 0);
            p.re = 1.0;
            p.im = 0.0;
            gkp_joint.push_back(std::move(p));
        }

        BellResult bell_verify(size_t rounds)
        {
            BellResult res;
            if (!use_gkp)
                return res; // 仅 GKP 编码支持完整梳态纠缠验证
            size_t z_same = 0, z_total = 0;
            size_t x_same = 0, x_total = 0;

            for (size_t r = 0; r < rounds; ++r)
            {
                // Z 基：直接测两个模式的奇偶。
                prepare_bell();
                gkp_h(0);
                gkp_cnot(0, 1); // |Φ+⟩
                int a = gkp_measure(0);
                int b = gkp_measure(1);
                ++z_total;
                if (a == b)
                    ++z_same;

                // X 基：H 变换后再测 Z 基。
                prepare_bell();
                gkp_h(0);
                gkp_cnot(0, 1);
                gkp_h(0);
                gkp_h(1);
                int xa = gkp_measure(0);
                int xb = gkp_measure(1);
                ++x_total;
                if (xa == xb)
                    ++x_same;
            }

            res.z_agreement = z_total ? double(z_same) / double(z_total) : 0.0;
            res.x_agreement = x_total ? double(x_same) / double(x_total) : 0.0;
            res.witness = 2.0 * (res.z_agreement + res.x_agreement) - 2.0;
            // CHSH 估计：W = 2 时对应 |S| = 2√2；线性外推（趋势量，非精确最优角）。
            res.chsh = 2.0 * std::sqrt(2.0) * std::max(0.0, res.witness) / 2.0;
            return res;
        }

        // 非破坏 Z 期望：光子数编码下 ⟨Z⟩ = P(0) − P(1)。
        double expectation_z(size_t qubit_id) override
        {
            std::lock_guard<std::mutex> lock(optics_mutex);
            ensure_mode(qubit_id);
            return modes[qubit_id].photon_number == 1 ? -1.0 : 1.0;
        }

        // 读取模式（供可视化 / 调试）。
        const GaussianMode &get_mode(size_t m) const { return modes.at(m); }
        size_t get_num_qubits() const override { return modes.size(); }
    };

}