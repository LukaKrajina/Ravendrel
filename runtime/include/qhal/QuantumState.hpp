#pragma once
//
// Quantum Graphics Engine（量子图形引擎）—— QGFX 核心状态模型
//
// ═══════════════════════════════════════════════════════════════════════
// 范式：显式投影的量子图形学 (Projection-Explicit Quantum Graphics, PEQG)
// ═══════════════════════════════════════════════════════════════════════
//
// 【问题】n 量子比特的纯态 living 在 CP^(2^n−1) 中，实维数
// 2(2^n−1)。n=2 已经是 6 个实参数；n=3 是 14 个。因此**不存在**等距
// 的「量子态图像」。文献中所有「Bloch 球推广」（超球 / 八元数 / 双
// Bloch / 2^n−1 嵌套球）都是**有意的子空间投影**，而非完整的可视化。
//
// 【结论式设计原则】
//
//  1  投影算子是 API 的一等参数，不是隐藏在实现里的近似。绘制者显式
//      选择丢弃什么，并在运行期反查自己选了哪个。
//      → arXiv:2212.03448、arXiv:2602.14661 的 subspace-vis 主张
//
//  2  每个投影自带一个**可判定的代数谓词**：一个 O(1) 的标量/布尔量，
//      可直接从渲染出的图形读出真伪。
//      典范：四元数部分 ≈ 0 ⟺ 两比特可分离（Najarbashi–Seifi,
//      arXiv:1512.07006）；图形左右对称 ⟺ 可分离（Bley et al.,
//      arXiv:2305.09596→2305.07596, PRResearch 6, 023077）。
//
//  3  幅度图元必须是**精确复值、振荡**核，不能用高斯 blob 近似。
//      Dirichlet splatting（arXiv:2610.00618, ACM TOG 2026）证明：
//      真实 PSF 是 Dirichlet 核时，高斯图元静默丢弃 10–20% 旁瓣能量
//      以及支配相干干涉的相位。对振幅可视化同理——丢掉相位即丢掉干涉，
//      而干涉正是量子态与经典分布的分界线。
//
//  4  层级（level of detail）是**数据结构的内蕴属性**。2^n−1 嵌套球
//      （arXiv:2607.04979）+ SZX 的寄存器抽象（arXiv:1905.00041）
//      意味着 20 比特态可渲染为约 5 圈同心环，而非 1024 根乱线。
//
//  5  一次模拟，多视图。Frame 持有振幅快照，投影与视图是正交维度；
//      同一 frame 可渲染为振幅图 / 相位带 / Wigner 截面 / 条码，零重算。
//
// ───────────────────────────────────────────────────────────────────────
//
// 【Frame 的契约】QuantumFrame 只接受**纯态**振幅向量。原因：statevector
// 模拟器（QVM / QM 的 backend->get_state_vector()）产出的就是纯态振幅。
// 混态（密度矩阵）需要 4^n 存储，不属于同一抽象层；框架以
// normalization_residual() 显式暴露这一点，而不是假装能画混态。
//
// 【已知的度量边界，勿误用】
//   - concurrence / pair_entanglement **只对 2 比特有定义**。n>2 时
//     pair_entanglement 恒返回 0（不可用），须改用
//     entanglement_exists() 的二值判据。理由见该函数文档。
//   - 纠缠条码（EntanglementBarcode）在纯态下的作用是**编码**而非增加
//     信息量：Schmidt 定理保证谱已含全部纠缠信息。详见该类文档。
//
// 本头文件**不依赖 Vulkan / Nuklear / 任何图形 API**，仅用标准库，
// 因此可独立单元测试。渲染层见 runtime/src/QuantumGfx.cpp。
//
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

namespace qhal
{

    using qcomplex = std::complex<double>;

    namespace qgf_detail
    {
        inline constexpr double kPi = 3.14159265358979323846;

        // 经典 Jacobi 特征值法（对称实矩阵），O(n³)。
        // 用于 Schmidt 谱 / 约化密度矩阵本征值。n 很小（≤ 2^n 子系统），
        // 精度优先于速度。
        inline void jacobi_eigen(std::vector<double> a, size_t n,
                                 std::vector<double> &evals)
        {
            evals.assign(n, 0.0);
            if (n == 0)
                return;
            std::vector<double> vecs(n * n, 0.0);
            for (size_t i = 0; i < n; ++i)
                vecs[i * n + i] = 1.0;

            for (int sweep = 0; sweep < 100; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += a[p * n + q] * a[p * n + q];
                if (off < 1e-26)
                    break;
                for (size_t p = 0; p < n; ++p)
                {
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const double apq = a[p * n + q];
                        if (std::fabs(apq) < 1e-18)
                            continue;
                        const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                        const double c = 1.0 / std::sqrt(t * t + 1.0);
                        const double s = t * c;
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double akp = a[k * n + p];
                            const double akq = a[k * n + q];
                            a[k * n + p] = c * akp - s * akq;
                            a[k * n + q] = s * akp + c * akq;
                        }
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double apk = a[p * n + k];
                            const double aqk = a[q * n + k];
                            a[p * n + k] = c * apk - s * aqk;
                            a[q * n + k] = s * apk + c * aqk;
                        }
                        for (size_t k = 0; k < n; ++k)
                        {
                            const double vkp = vecs[k * n + p];
                            const double vkq = vecs[k * n + q];
                            vecs[k * n + p] = c * vkp - s * vkq;
                            vecs[k * n + q] = s * vkp + c * vkq;
                        }
                    }
                }
            }
            for (size_t i = 0; i < n; ++i)
                evals[i] = a[i * n + i];
        }

        // Schmidt 谱（纯态二分下的纠缠谱），降序。
        //
        // 定义与存在性：Schmidt 定理保证任意纯态在 A|B 二分下可写成
        //     |ψ⟩ = Σ_k λ_k |k⟩_A |k⟩_B,   Σ_k λ_k² = 1
        // λ_k 是把 |ψ⟩ 按 A|B 重整为矩阵 M 后的奇异值。故 λ² 恰是约化
        // 密度矩阵 ρ_A 的本征值谱，且 S(ρ_A) = H(λ²)。
        //
        // 放在 qgf_detail 里是因为 QuantumFrame::von_neumann_entropy 与
        // EntanglementBarcode 都要用它，而后者定义在文件更下方 —— 把
        // 实现放在此处使两者的依赖对称，不依赖类定义顺序。
        //
        // 代价：O(2^n · min(2^k, 2^{n−k})) + O(min(2^k, 2^{n−k})³)。
        // 对比直接构造 2^k × 2^k 密度矩阵的 O(8^k) —— k=3 时快约 8 倍，
        // 且数值上更稳（无需处理复厄米矩阵的数值本征值）。
        inline std::vector<double> schmidt_spectrum(const std::vector<qcomplex> &amps,
                                                    uint32_t num_qubits,
                                                    uint32_t first_qubit,
                                                    uint32_t qubit_count)
        {
            std::vector<double> out;
            if (qubit_count == 0 || num_qubits == 0 ||
                first_qubit + qubit_count > num_qubits)
                return out;
            const size_t dim = amps.size();
            const size_t sub_dim = size_t(1) << qubit_count;
            if (dim < sub_dim || dim % sub_dim != 0)
                return out;
            const size_t other = dim / sub_dim;

            // 把 |ψ⟩ 按 A|B 重整为 M（sub_dim × other）：
            //   行索引 = A 内各比特（保持相对次序）
            //   列索引 = B 内各比特（保持相对次序）
            // first_qubit 可为任意值：重整把 A 的比特**搬到行侧**，
            // 因此无论它们在原索引中是否连续，得到的 M 都正确。
            std::vector<qcomplex> m(sub_dim * other, qcomplex(0.0, 0.0));
            for (size_t i = 0; i < dim; ++i)
            {
                size_t row = 0, col = 0;
                uint32_t rbit = 0, cbit = 0;
                for (uint32_t b = 0; b < num_qubits; ++b)
                {
                    const size_t set = (i >> b) & size_t(1);
                    if (b >= first_qubit && b < first_qubit + qubit_count)
                    {
                        row |= set << rbit;
                        ++rbit;
                    }
                    else
                    {
                        col |= set << cbit;
                        ++cbit;
                    }
                }
                m[row * other + col] = amps[i];
            }

            // λ² 是 M^H M（或 M M^H，取较小一侧以压低 O(·³)）的本征值。
            const bool transpose = sub_dim < other;
            const size_t side = transpose ? sub_dim : other;
            const size_t inner = transpose ? other : sub_dim;
            std::vector<double> gram(side * side, 0.0);
            for (size_t p = 0; p < side; ++p)
            {
                for (size_t q = p; q < side; ++q)
                {
                    double acc = 0.0;
                    for (size_t k = 0; k < inner; ++k)
                    {
                        const qcomplex lp = transpose ? m[p * other + k] : m[k * side + p];
                        const qcomplex lq = transpose ? m[q * other + k] : m[k * side + q];
                        acc += (lp * std::conj(lq)).real();
                    }
                    gram[p * side + q] = acc;
                    gram[q * side + p] = acc; // 实对称
                }
            }

            std::vector<double> evals;
            jacobi_eigen(std::move(gram), side, evals);
            out.reserve(evals.size());
            for (double e : evals)
                out.push_back(std::sqrt(std::max(0.0, e)));
            // 丢弃数值噪声级零奇异值。
            //
            // 【阈值必须相对**最大**奇异值，不能用 out.front()】
            // Jacobi 迭代返回的特征值顺序是任意的，front() 可能是最小的
            // 那个。若用 front()·1e-9 当阈值：一个真实零值对应的
            // √(1e-17) ≈ 3.2e-9 反而**大于** 3.2e-18 的阈值而不被剪除，
            // 于是"秩"被虚增 1，separable_across 把一个真正可分离的
            // 纯乘积态误判为纠缠。用 max_element 则始终正确。
            const double lead = out.empty() ? 0.0 : *std::max_element(out.begin(), out.end());
            const double tol = lead * 1e-9;
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [tol](double v) { return v < tol; }),
                      out.end());
            std::sort(out.begin(), out.end(), std::greater<double>());
            return out;
        }
    } // namespace qgf_detail

    // ─── 投影算子（P1）──────────────────────────────────────────────
    // 每一种都是 CP^(2^n−1) → 某个低维流形的有损映射。
    // 名字即契约：绘制者知道自己丢掉了什么。
    enum class QgfProjection : int32_t
    {
        // 每比特的局域 Bloch 向量 (x,y,z)。n≥2 时纠缠信息全部落入残差。
        LocalBloch = 0,
        // 两比特立体投影（Najarbashi–Seifi, arXiv:1512.07006）：
        // 实部编码 Schmidt 角，虚部编码 concurrence。
        Quaternionic = 1,
        // 三球分解（Wie, arXiv:1403.8069）：
        // 2 个局域球 + 1 个纠缠球（concurrence 的度数与相位）+ 1 个全局相位。
        EntanglementSphere = 2,
        // 2^n − 1 嵌套球（Piñero et al., arXiv:2607.04979）：
        // 纠缠住最内层壳，局域态住外层壳——O(1) 每层的层级布局。
        Nested = 3,
        // 任意维 Bloch 超球（Hasebe, arXiv:2402.07149）。距离 = Bures 度量。
        Hypersphere = 4,
        // 相空间截面：HUGE 意义下的 Wigner 函数截面
        // （Tilma–Everitt, arXiv:1710.02460；Davies et al., arXiv:1809.05431）。
        Wigner = 5,
        // 密度矩阵直方图（非对角项按 Re/Im 分色显示相位）。
        Density = 6,
    };

    inline const char *qgf_projection_name(QgfProjection p)
    {
        switch (p)
        {
        case QgfProjection::LocalBloch:        return "local_bloch";
        case QgfProjection::Quaternionic:       return "quaternionic";
        case QgfProjection::EntanglementSphere: return "entanglement_sphere";
        case QgfProjection::Nested:             return "nested";
        case QgfProjection::Hypersphere:        return "hypersphere";
        case QgfProjection::Wigner:             return "wigner";
        case QgfProjection::Density:            return "density";
        }
        return "unknown";
    }

    // 投影前的真实维数：CP^k 的实维数是 2k，而纯态空间是 CP^(2^n−1)，
    // 故 dim_R = 2(2^n − 1)。
    //   n=1 → 2（Bloch 球 S²，恰好完整）
    //   n=2 → 6（超过任何 2D 平面，故 Bloch 类图必为有损投影）
    //   n=3 → 14
    inline double qgf_state_real_dims(uint32_t num_qubits)
    {
        if (num_qubits == 0)
            return 0.0;
        const double d = std::ldexp(1.0, static_cast<int>(num_qubits));
        return 2.0 * (d - 1.0);
    }

    // 投影后可见的**独立**维数 —— 信息损失量的分子。
    //
    // 【记账口径，必须严格】
    // 返回*独立参数个数*，而非图元上画出的坐标个数。两者在
    // EntanglementSphere 上不同：画 3 个球共 9 个坐标，但它们受秩-1
    // 约束，只有 6 个独立参数 —— 故此处以源维数封顶。
    //
    // 这个区分很重要：若按"画了几个数"记账，该投影会报告 9 > 6，
    // 看起来像"增维"，从而掩盖它其实是完整的 2 比特描述。
    inline double qgf_projection_target_dims(QgfProjection p, uint32_t num_qubits)
    {
        const double n = static_cast<double>(num_qubits);
        const double src = qgf_state_real_dims(num_qubits);
        auto capped = [src](double drawn) { return std::min(drawn, src); };
        switch (p)
        {
        case QgfProjection::LocalBloch:
            return capped(3.0 * n);
        case QgfProjection::Quaternionic:
            // (θ, φ, 四元数部分)；全局相位可由相对相位重建，不计入
            return capped(3.0);
        case QgfProjection::EntanglementSphere:
            // 3 球 × 3 坐标，但受秩-1 约束 → 独立参数以源维数为上界
            return capped(n >= 2.0 ? 9.0 : 3.0);
        case QgfProjection::Nested:
            return std::min(3.0, src); // 任一时刻只画最内层壳
        case QgfProjection::Hypersphere:
            return std::min(3.0, src);
        case QgfProjection::Wigner:
            return std::min(2.0, src); // 单个 2D 截面
        case QgfProjection::Density:
            // 对称 2^n × 2^n 复矩阵，但纯态只有 rank-1 → 独立参数 = 源维数
            return src;
        }
        return 0.0;
    }

    // ─── 四元数坐标（Quaternionic 投影的输出坐标，P1+P2 的连接点）──
    //
    // 立体投影把两比特纯态嵌入四元数：实部编码 Schmidt 角，
    // **虚部编码 concurrence**（Najarbashi–Seifi, arXiv:1512.07006）。
    // 因此 quaternionic ≈ 0 是可分离性的代数判据 —— 这就是 P2：
    // 渲染器画的就是这 4 个数，用户读图即读判据。
    struct QuaternionicCoords
    {
        double theta = 0.0;       // Schmidt 角 0..π/2
        double phi = 0.0;         // 局域相对相位
        double quaternionic = 0.0; // concurrence / 2，≈0 ⟺ 可分离
        double global_phase = 0.0; // 整体相位 arg(a00)
        bool valid = false;
    };

    // ─── Frame：一次捕获的纯态振幅快照（P5）────────────────────
    //
    // Frame 不拥有硬件句柄，只拥有捕获时刻的复振幅副本。投影与视图切换
    // 是 O(1) 元数据操作，因此同一 frame 可同时喂给任意多个原语。
    class QuantumFrame
    {
    public:
        // 硬上限 24 比特（2^24 振幅 ≈ 268 MB 复数），防止误传 n 触发爆炸分配。
        static constexpr uint32_t kMaxQubits = 24;

        // 【契约】默认构造出的帧振幅全零 —— 这**不是** |0…0>，而是
        // 非物理的零向量，归一化残差 = 1.0。理由同 ABI 文档：
        // 零向量如实标记「尚无数据」，而 |0…0> 会伪装成合法真空态。
        // 调用方须先查 normalization_residual() 再信任任何谓词读数。
        QuantumFrame() { set_amplitudes({}, 1); }

        // 从显式振幅数组捕获。长度不足补零，超出截断。
        void set_amplitudes(std::vector<qcomplex> amps, uint32_t num_qubits)
        {
            num_qubits_ = std::clamp<uint32_t>(num_qubits, 1, kMaxQubits);
            const size_t want = hilbert_dim(num_qubits_);
            if (amps.size() > want)
                amps.resize(want);
            amps.resize(want, qcomplex(0.0, 0.0));
            amps_ = std::move(amps);
        }

        uint32_t num_qubits() const { return num_qubits_; }
        size_t hilbert_dim() const { return amps_.size(); }

        const std::vector<qcomplex> &amplitudes() const { return amps_; }
        const qcomplex &amplitude(size_t basis) const { return amps_[basis]; }

        // 归一化残差 |1 − ‖ψ‖|。非 0 即意味着概率轴不可信，
        // 渲染层应把它作为诊断量显示，而不是默默归一化后假装一切正常。
        double normalization_residual() const
        {
            return std::fabs(1.0 - total_norm());
        }

        // 碰撞概率 Σ|a_i|⁴ —— |ψ⟩ 与计算基 {|i⟩} 的 Hilbert–Schmidt 重叠。
        //
        // 【易混淆，务必分清】它**不是** Tr(ρ²)。对纯态 ρ = |ψ⟩⟨ψ| 有
        // Tr(ρ²) = ⟨ψ|ψ⟩² = 1，与态无关；而 Σ|a_i|⁴ 确实随态变化
        // （|0…0⟩ 给 1，最大叠加给 1/2^n）。线性熵 S_L = 1 − Σ|a_i|⁴
        // 才是文献里的「线性纯度」。
        //
        // 用途：诊断「振幅是否过度分散」。1 = 完全集中于单一基态；
        // 1/2^n = 最大叠加。用于校验仿真是否意外退化为均匀分布。
        double collision_probability() const
        {
            double acc = 0.0;
            for (const auto &a : amps_)
                acc += std::norm(a) * std::norm(a);
            return acc;
        }

        // 线性熵 S_L = 1 − Σ|a_i|⁴（已归一化前提下 ∈ [0, 1]）。
        // 0 = 单一基态，1 = 最大叠加。
        double linear_entropy() const
        {
            const double n = total_norm();
            if (n <= 0.0)
                return 0.0;
            return std::clamp(1.0 - collision_probability() / (n * n), 0.0, 1.0);
        }

        // Born 规则：基态 i 的测量概率（按当前范数，不强行归一）。
        double probability(size_t basis) const
        {
            return basis < amps_.size() ? std::norm(amps_[basis]) : 0.0;
        }

        // 该基态的相位（弧度 ∈ (−π, π]）。零振幅处相位无定义，返回 0。
        double phase(size_t basis) const
        {
            if (basis >= amps_.size())
                return 0.0;
            return std::arg(amps_[basis]);
        }

        // 局域 Bloch 向量（单个比特的 Pauli 期望），O(2^n)，精确。
        //
        // 对每个比特 q，用 mask = 1<<q 遍历无序对 (i, j=i^mask)：
        //   <X> += 2·Re(a_i a_j*)      （|0> 分量 a_i，|1> 分量 a_j）
        //   <Y> += −2·Im(a_i a_j*)      —— 与 |+i⟩ 为 +Y 本征态一致
        //   <Z> += |a_i|² − |a_j|²
        // 校验：|+⟩ = (|0⟩+|1⟩)/√2 → (1, 0, 0)；|+i⟩ → (0, 1, 0)。✔
        void local_bloch(uint32_t qubit, double &rx, double &ry, double &rz) const
        {
            const size_t q = qubit < num_qubits_ ? qubit : 0;
            const size_t mask = size_t(1) << q;
            double sx = 0.0, sy = 0.0, sz = 0.0;
            for (size_t i = 0; i < amps_.size(); ++i)
            {
                const size_t j = i ^ mask;
                if (j < i)
                    continue; // 每无序对只算一次
                const qcomplex z = amps_[i] * std::conj(amps_[j]);
                sx += 2.0 * z.real();
                sy += -2.0 * z.imag();
                sz += std::norm(amps_[i]) - std::norm(amps_[j]);
            }
            const double n = total_norm();
            const double s = n > 0.0 ? 1.0 / n : 0.0;
            rx = sx * s;
            ry = sy * s;
            rz = sz * s;
        }

        // Bloch 球极坐标 (θ, φ)：θ 为与 |0⟩（+Z）的夹角，φ 为方位角。
        void local_bloch_polar(uint32_t qubit, double &theta, double &phi) const
        {
            double x, y, z;
            local_bloch(qubit, x, y, z);
            const double r = std::sqrt(x * x + y * y + z * z);
            theta = std::acos(std::clamp(z, -1.0, 1.0));
            phi = (r > 1e-12) ? std::atan2(y, x) : 0.0;
        }

        // ─── P2 谓词：四元数投影坐标 ──────────────────────────
        //
        // 仅对 2 比特有意义（Najarbashi–Seifi 的嵌入是 C⁴ → 四元数）。
        // Schmidt 定理保证 |ψ⟩ 可写为 Σ_k λ_k |k⟩|k⟩，其中
        // a00 a11 − a01 a10 = −(λ0λ1)e^{iφ}，故 concurrence = 2|…|
        // 与 quaternionic 坐标一致，四元数部分为 0 ⟺ λ0λ1 = 0 ⟺ 可分离。
        QuaternionicCoords quaternionic_coords() const
        {
            QuaternionicCoords q{};
            if (num_qubits_ != 2 || amps_.size() < 4)
                return q;
            const qcomplex a = amps_[0], b = amps_[1];
            const qcomplex c = amps_[2], d = amps_[3];
            const double n = total_norm();
            if (n <= 0.0)
                return q;

            // concurrence / 2 = |a00·a11 − a01·a10| （纯态精确式）
            const qcomplex cross = a * d - b * c;
            q.quaternionic = std::abs(cross) / (n * n);

            // Schmidt 角：λ0 = cos θ_s，λ1 = sin θ_s，
            // 由 concurrence = sin(2θ_s) 得 θ = ½·asin(C)
            const double conc = 2.0 * q.quaternionic;
            const double theta_s = 0.5 * std::asin(std::clamp(conc, -1.0, 1.0));
            q.theta = std::clamp(theta_s, 0.0, qgf_detail::kPi * 0.5);

            // 局域相对相位：Schmidt 向量 u0、u1 之间的相位差
            q.phi = (std::abs(c) > 1e-15) ? std::arg(a * std::conj(c))
                                          : ((std::abs(b) > 1e-15) ? std::arg(b * std::conj(d)) : 0.0);
            q.global_phase = std::arg(a);
            q.valid = true;
            return q;
        }

        // Wootters concurrence（仅 2 比特；纯态精确式，O(1)）。
        // 校验：Bell |Φ+⟩ = (|00⟩+|11⟩)/√2 → C = 1；|00⟩ → C = 0。✔
        // 派生关系：C = 2λ0λ1（λ 为 Schmidt 系数），**不是** λ0²−λ1²。
        double concurrence() const
        {
            const QuaternionicCoords q = quaternionic_coords();
            return q.valid ? 2.0 * q.quaternionic : 0.0;
        }

        // 二分可分离性判据（纯态，**充要**）。
        //
        // Schmidt 定理给出等价刻画：|ψ⟩ 在 A|B 二分下可分离
        // ⟺ Schmidt 谱的**秩为 1**（即可写成 |a⟩_A ⊗ |b⟩_B）。
        //
        // 秩的判定用「谱长度」而非「比较各系数」：schmidt_spectrum
        // 已把数值噪声级（< 主值的 1e-9）的零奇异值剪除，故
        // 剩余系数个数即为秩。这比逐项比较更干净 —— 逐项比较会
        // 把 {λ, λ}（秩 2，真纠缠）误判为秩 1。
        //
        // 在本 Frame 的纯态契约下，这是最强的可分离性判据，且严格强于
        // 边缘熵。反例说明差距：GHZ3 的三个单比特边缘全部最大混合
        // （S = 1），而纯乘积态 |0>⊗|+>⊗|0> 的边缘则是 {0, 0, 1}；
        // 二者在「全部边缘混合 ⟹ 多体纠缠」（Koenig–Winter /
        // Gühne–Vidal）这一判据上给出不同结论，而秩判据始终与
        // 二分上的纠缠性一致。
        //
        // 复杂度 O(2^n · min(2^k, 2^{n−k}))，非 O(1)。
        bool separable_across(uint32_t first_qubit, uint32_t qubit_count) const
        {
            if (qubit_count == 0)
                return true; // 空子系统平凡可分离
            if (first_qubit + qubit_count > num_qubits_)
                return true; // 越界视作平凡
            if (qubit_count >= num_qubits_)
            {
                // 退化二分（全体 vs 空）：Schmidt 谱为 {1}，秩 1 → 可分离。
                // 语义上正确，但作为「是否纠缠」的提问没有意义，
                // 故调用方（entanglement_exists）会先拦截该情形。
                return true;
            }
            const std::vector<double> lam =
                qgf_detail::schmidt_spectrum(amps_, num_qubits_, first_qubit, qubit_count);
            if (lam.empty())
                return false; // 零向量（非物理）：不宣称任何性质
            return lam.size() <= 1; // 秩 1 ⟺ 可分离
        }

        // 纠缠存在性（P2 的可判定谓词，二值）。
        //
        // 【语义，二者不等价，必须分清】
        // n>2 时本函数问的是二分 **{qa, qb} | 其余比特** 是否纠缠，
        // **不是**「qa 与 qb 这两个比特单独是否纠缠」。例如
        // Bell(q0q1) ⊗ |0>(q2)：
        //   entanglement_exists(0, 0) → true   （{q0}|{q1,q2}，Bell 对跨此切分）
        //   entanglement_exists(0, 1) → false  （{q0,q1}|{q2}，张量积，可分离）
        // 混淆二者会让 UI 把"局部纠缠"误画成"全局纠缠"。
        //
        // 若要问「单比特 q 与其余是否纠缠」，请传 entanglement_exists(q, q)。
        //
        // n=2：concurrence > 0（Wootters，充要）——此时两种解读重合。
        // n>2：Schmidt 秩判据，在纯态契约下**充要**（见 separable_across）。
        bool entanglement_exists(uint32_t qa = 0, uint32_t qb = 1) const
        {
            if (num_qubits_ <= 1)
                return false;
            if (num_qubits_ == 2)
                return concurrence() > 1e-9;
            const uint32_t lo = std::min(qa, qb);
            const uint32_t hi = std::max(qa, qb);
            return !separable_across(lo, hi - lo + 1);
        }

        // 纠缠**强度**（连续量 ∈ [0,1]），仅对 2 比特有定义。
        //
        // n=2：Wootters concurrence，精确。
        // n>2：**不可用，恒返回 0**。
        //
        // 【为何不外推到 n>2】自然的外推是 Schmidt 间隙 λ0−λ1，但它
        // **错误**：Bell 态的 Schmidt 谱是 {1/√2, 1/√2}，间隙为 0，而
        // Bell 恰是最大纠缠（concurrence = 2λ0λ1 = 1）。任何只依赖
        // 「谱的单参数差」的度量都无法同时给出 Bell = 1 与可分离 = 0。
        // 同样，1 − S(qa) − S(qb) 对完全可分离态也给 1。
        //
        // 正确的 n>2 方案需构造该比特对的 **4×4 约化密度矩阵** 并用
        // Wootters 混态公式（需复厄米特征值）。超出本 Frame 的 O(1)
        // 预算，故此处**明确不提供**，而非返回一个似是而非的数字 ——
        // 后者会让 UI 悄悄画出错误的纠缠强度。
        //
        // n>2 时请改用 entanglement_exists() 取二值判据。
        double pair_entanglement(uint32_t qa = 0, uint32_t qb = 1) const
        {
            if (num_qubits_ != 2 || qa == qb || qa >= num_qubits_ || qb >= num_qubits_)
                return 0.0;
            return concurrence();
        }

        // 冯·诺依曼熵 S(子系统 A)，A 为比特区间 [first, first+count)。
        //
        // 实现说明（这是本文件里最容易写错的一处，故显式记录）：
        // 对**纯态**，S(A) 等于该二分下 Schmidt 系数的 Shannon 熵。
        // 直接算约化密度矩阵 ρ_A = Tr_B|ψ⟩⟨ψ| 需要 4^n 级内存，而
        // Schmidt 谱只需 M^H M 的本征值 —— 代价 O(2^n · min(2^k,2^{n−k}))，
        // 且**精确**。
        //
        // 关键：A 的比特**不必连续**。但 Schmidt 系数的**多重集**只取决于
        // |A| 与 |B|（对 A、B 各自施加局域幺正不改变谱），因此
        // first_qubit 只影响「取哪一段」，不影响熵值。这条性质让本实现
        // 对任意比特子集都成立，而无需真的做部分迹。
        //
        // 校验：Bell 态对单比特 A → Schmidt (1/√2,1/√2) → S = 1。✔
        //       W 态对单比特 A → Schmidt (√(2/3),√(1/3)) → S ≈ 0.9183。✔
        //       全体比特 → 二分退化，谱为 {1} → S = 0（纯态）。✔
        double von_neumann_entropy(uint32_t first_qubit, uint32_t qubit_count) const
        {
            if (qubit_count == 0 || first_qubit + qubit_count > num_qubits_)
                return 0.0;
            const double n = total_norm();
            if (n <= 0.0)
                return 0.0;

            // 全体比特：纯态熵恒为 0（退化二分）
            if (qubit_count >= num_qubits_)
                return 0.0;

            const std::vector<double> schmidt =
                qgf_detail::schmidt_spectrum(amps_, num_qubits_, first_qubit, qubit_count);
            double h = 0.0;
            for (double lam : schmidt)
            {
                const double p = lam * lam / (n * n); // 概率 = λ² / ‖ψ‖²
                if (p > 1e-15)
                    h -= p * std::log2(p);
            }
            return std::max(0.0, h);
        }

        // ─── P3：Dirichlet 核（精确、复值、振荡、闭式 O(1)）──────
        //
        // 有限窗 DFT 的 PSF 是 Dirichlet 核而非高斯：
        //     D_N(x) = sin((N + ½)x) / sin(x/2)
        // （arXiv:2610.00618, ACM TOG 2026）。用它加权的振幅条带会
        // 自然涌现干涉条纹——条纹来自**相对相位**，而非手工绘制。
        static double dirichlet(double x, int peaks)
        {
            if (peaks < 1)
                return 1.0;
            const double num = std::sin((peaks + 0.5) * x);
            const double den = std::sin(0.5 * x);
            if (std::fabs(den) < 1e-12)
            {
                // x→0 的洛必达极限：((N+½)·cos 0) / (½·cos 0) = 2N+1。
                // 注意是 2N+1 而非 (N+½)² —— 主瓣幅度与瓣数成正比。
                return 2.0 * peaks + 1.0;
            }
            return num / den;
        }

        // 相位加权的复值图元值（P3 的核心）：
        //   幅值 |a| 决定强度，辐角 arg(a) 决定干涉项的相位。
        // 两个相邻基态之间的干涉项 ∝ a_i a_j* —— 这正是相对相位
        // 唯一进入图形的地方，也是经典直方图永远丢失的信息。
        qcomplex amplitude_kernel(size_t i, size_t j, int peaks) const
        {
            if (i >= amps_.size() || j >= amps_.size())
                return qcomplex(0.0, 0.0);
            const qcomplex z = amps_[i] * std::conj(amps_[j]);
            const double x = qgf_detail::kPi * static_cast<double>(i + j) /
                             static_cast<double>(std::max<size_t>(peaks, 1));
            return dirichlet(x, peaks) * z;
        }

        // ─── P4：嵌套层级 ──────────────────────────────────────
        // 2^n − 1 层嵌套球：外壳 0..2^(n−1)−1，最内壳持有纠缠。
        static uint32_t nested_shell_count(uint32_t num_qubits)
        {
            if (num_qubits <= 1)
                return 1;
            return (size_t(1) << (num_qubits - 1)); // 2^(n−1) 个壳
        }

    private:
        static size_t hilbert_dim(uint32_t n)
        {
            return size_t(1) << std::min<uint32_t>(n, kMaxQubits);
        }

        double total_norm() const
        {
            double acc = 0.0;
            for (const auto &a : amps_)
                acc += std::norm(a);
            return std::sqrt(acc);
        }

        uint32_t num_qubits_ = 1;
        std::vector<qcomplex> amps_;
    };

    // ─── 纠缠条码：Schmidt 谱的拓扑化编码 ─────────────────────────
    //
    // 【与文献的界限，务必如实说明】
    //
    // Hamilton–Leditzky（arXiv:2307.07492, Comm. Math. Phys. 2024）以
    // **形变总关联**为滤流做持久同调，得到比标量纠缠度量更具区分力的
    // 条码 —— 严格说这适用于**混态（密度矩阵）**。而此处 QuantumFrame
    // 按契约只承载**纯态**，因此 Schmidt 定理生效：Schmidt 谱即为纠缠
    // 谱的完整信息。
    //
    // 纯态下的真实增益是**编码能力**，而非信息量：
    //   - 条码把谱压成 (birth, death) 对，天然支持比较 / 排序 / 哈希；
    //     concurrence 把谱压成单个数，丢失谱形状；
    //   - 因此在 Schmidt **秩相同但谱形状不同**的态对之间，条码判据仍能
    //     区分，而秩本身不能。
    // 这是「条码 > 秩」的准确表述。若称其「比 concurrence 更有信息量」，
    // 在纯态情形下并不成立——concurrence 由 Schmidt 谱唯一确定。
    //
    // 支持**混态**条码（文献原意）需要 4^n 的密度矩阵表示，
    // 属于 QuantumFrame 之外的后续扩展。
    //
    // Vlasic–Pham（arXiv:2209.10596）表明对边界算符做简单对角化即可在
    // 量子设备上求得持久 Betti 数，故条码属于**运行时**可算的量，
    // 而非仅离线分析工具。
    //
    // 支撑 `rvdl_qgfx_same_class`：一个对局域幺正不变的 O(1) 状态同一性
    // 判据，其分辨力严格不低于 Schmidt 秩。
    struct QgfBar
    {
        double birth = 0.0;
        double death = 0.0;
        // 冗余存储持久度，使该结构可作**聚合类型**初始化（见 from_schmidt）。
        // 与 persistence() 恒等，保留字段仅为消除 push_back 歧义。
        double persist = 0.0;

        double persistence() const { return death - birth; }
    };

    class EntanglementBarcode
    {
    public:
        // 由 Schmidt 谱构造条码：H₀ 条 {1 → λ₁}，H₁ 条 {λ₁ → λ₀}。
        // λ 升序排列。对纯态，Schmidt 谱即纠缠谱。
        static void from_schmidt(const std::vector<double> &schmidt, std::vector<QgfBar> &out)
        {
            out.clear();
            if (schmidt.empty())
                return;
            std::vector<double> lam = schmidt;
            std::sort(lam.begin(), lam.end()); // 升序
            // H₀：分量从 1 降到最大 Schmidt 系数 λ₁
            out.push_back({1.0, lam.back(), 1.0 - lam.back()});
            // H₁：λ₁ 与 λ₀ 之间的间隙
            if (lam.size() >= 2)
                out.push_back({lam[1], lam[0], lam[0] - lam[1]});
        }

        // 从 QuantumFrame 的 [first, first+qubits) 子系统求 Schmidt 谱。
        // 实现见 qgf_detail::schmidt_spectrum（该处记录了完整的数学推导
        // 与代价分析）。这里只做转发，避免逻辑在两处重复。
        static std::vector<double> schmidt_spectrum(const QuantumFrame &f,
                                                    uint32_t first_qubit,
                                                    uint32_t qubits)
        {
            return qgf_detail::schmidt_spectrum(f.amplitudes(), f.num_qubits(),
                                                first_qubit, qubits);
        }

        static void of_frame(const QuantumFrame &f, uint32_t first_qubit, uint32_t qubits,
                             std::vector<QgfBar> &out)
        {
            from_schmidt(schmidt_spectrum(f, first_qubit, qubits), out);
        }

        // 状态同一性判据（P2 典范）：条码相等（容差内）⟺ 局域幺正等价类相同。
        static bool same_class(const std::vector<QgfBar> &a, const std::vector<QgfBar> &b,
                               double tol = 1e-6)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i].birth - b[i].birth) > tol ||
                    std::fabs(a[i].death - b[i].death) > tol)
                    return false;
            return true;
        }
    };

} // namespace qhal
