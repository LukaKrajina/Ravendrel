#pragma once
#include <cstddef>
#include <vector>
#include <string>
#include <complex>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>

#include "Export.hpp"

namespace qhal
{

    class RVDL_RT_API IQuantumBackend
    {
    public:
        virtual ~IQuantumBackend() = default;
        virtual void allocate_qubits(size_t num_qubits) = 0;
        // 按具体 id 分配单个 qubit（供全局 id 复用分配器使用）。
        // 默认实现 = 确保容量 >= id+1；维护空闲位图的后端（如 QVM）应重写，
        // 以便复用已释放 id 时重新标记该 id 为已分配。
        virtual void allocate_qubit(size_t qubit_id)
        {
            allocate_qubits(qubit_id + 1);
        }
        virtual void release_qubit(size_t qubit_id) = 0;
        virtual void lock_hardware_id(size_t qubit_id) = 0;
        virtual void unlock_hardware_id(size_t qubit_id) = 0;
        virtual int measure(size_t qubit_id) = 0;

        // 非破坏 Z 期望 ⟨Z⟩ = P(0) - P(1) ∈ [-1, 1]。
        // 默认实现用单次坍缩近似（2*measure-1），会破坏态；
        // 维护态矢量的后端（如 QVM）应重写为真正的非破坏测量。
        // 用于「离散/连续双通道」中的连续期望通道（流匹配软信号）。
        virtual double expectation_z(size_t qubit_id)
        {
            return 2.0 * measure(qubit_id) - 1.0;
        }

        virtual void apply_h(size_t qubit_id) {}
        virtual void apply_x(size_t qubit_id) = 0;
        // 精确 Z 门。默认用 Z = H·X·H（无全局相位）；
        // 维护态矢量的后端（如 QVM）应重写为直接相位翻转以获得更高效率。
        virtual void apply_z(size_t qubit_id)
        {
            apply_h(qubit_id);
            apply_x(qubit_id);
            apply_h(qubit_id);
        }
        // Y 门。精确实现 Y = Rz(-π/2)·X·Rz(π/2)（无全局相位差，steer 语义下相位可观测）。
        virtual void apply_y(size_t qubit_id)
        {
            apply_rz(qubit_id, -M_PI / 2.0);
            apply_x(qubit_id);
            apply_rz(qubit_id, M_PI / 2.0);
        }
        virtual void apply_rz(size_t qubit_id, double angle) = 0;
        // Rx(θ) = H·Rz(θ)·H（到全局相位）；维护态矢量的后端应重写为精确矩阵。
        virtual void apply_rx(size_t qubit_id, double angle)
        {
            apply_h(qubit_id);
            apply_rz(qubit_id, angle);
            apply_h(qubit_id);
        }
        // Ry(θ) = Rz(-π/2)·Rx(θ)·Rz(π/2)（到全局相位）。
        virtual void apply_ry(size_t qubit_id, double angle)
        {
            apply_rz(qubit_id, -M_PI / 2.0);
            apply_rx(qubit_id, angle);
            apply_rz(qubit_id, M_PI / 2.0);
        }
        // S = Rz(π/2)，T = Rz(π/4)（到全局相位）。
        virtual void apply_s(size_t qubit_id)
        {
            apply_rz(qubit_id, M_PI / 2.0);
        }
        virtual void apply_t(size_t qubit_id)
        {
            apply_rz(qubit_id, M_PI / 4.0);
        }
        virtual void apply_cnot(size_t control, size_t target) = 0;
        virtual void apply_toffoli(size_t control1, size_t control2, size_t target) = 0;

        // 振幅编码：直接设置态矢量（amplitude encoding，N 维 → ⌈log2 N⌉ qubit）。
        // 默认空实现（真实 QM 不支持直接写振幅，需门序列制备）；QVM 重写为直接
        // 设置 dense_state / MPS。amps.size() == 2^num_qubits。
        virtual void prepare_amplitudes(const std::vector<std::complex<double>> &amps,
                                        size_t num_qubits)
        {
            (void)amps;
            (void)num_qubits;
        }

        virtual void apply_swap(size_t a, size_t b)
        {
            if (a == b)
                return;
            apply_cnot(a, b);
            apply_cnot(b, a);
            apply_cnot(a, b);
        }

        // ─── 受控门（可逆编织 @[steer] 的运行时实现，用基础门精确分解）───
        // 约定：Rz(θ) = diag(1, e^{iθ})（asymmetric，与 QVM/IdealStateCore 一致）。
        // 受控 Rz（精确）：CRz(θ) = Rz(θ/2)[t] · CNOT(c,t) · Rz(-θ/2)[t] · CNOT(c,t) · Rz(θ/2)[c]
        //（末项把 CNOT 路径的相对相位补回，使结果严格等于 diag(1,1,1,e^{iθ})）。
        virtual void apply_crz(size_t control, size_t target, double angle)
        {
            apply_rz(target, angle / 2.0);
            apply_cnot(control, target);
            apply_rz(target, -angle / 2.0);
            apply_cnot(control, target);
            apply_rz(control, angle / 2.0);
        }
        // 受控 H（精确）：H = Rz(π/2)·Rx(π/2)·Rz(π/2) 且 Rx(π/2) = e^{-iπ/4}·(H·Rz(π/2)·H)，
        // CH = CRz(π/2)·H·CRz(π/2)·H·CRz(π/2) · Rz(-π/4)[c]（尾项修正两分支相对相位）。
        virtual void apply_ch(size_t control, size_t target)
        {
            apply_crz(control, target, M_PI / 2.0);
            apply_h(target);
            apply_crz(control, target, M_PI / 2.0);
            apply_h(target);
            apply_crz(control, target, M_PI / 2.0);
            apply_rz(control, -M_PI / 4.0);
        }
        // 受控 X（控制位导引 X）= CNOT；受控 CNOT = Toffoli
        virtual void apply_cx(size_t control, size_t target)
        {
            apply_cnot(control, target);
        }
        // 受控 swap（Fredkin）：CSWAP = CNOT(b,a) · Toffoli(c,a,b) · CNOT(b,a)
        virtual void apply_cswap(size_t control, size_t a, size_t b)
        {
            if (a == b)
                return;
            apply_cnot(b, a);
            apply_toffoli(control, a, b);
            apply_cnot(b, a);
        }
        // 受控 Toffoli（C³X，精确）：无 ancilla 分解，Qiskit C3XGate 电路（synth_c3x，
        // 14 CX + 15 个 P(±π/8) + 2 H；P(θ)=diag(1,e^{iθ}) 恰为本约定 Rz(θ)）。
        // c 为额外控制位，a/b 为原 Toffoli 控制位，t 为目标。
        virtual void apply_c_toffoli(size_t c, size_t a, size_t b, size_t t)
        {
            apply_h(t);
            apply_rz(c, M_PI / 8.0);
            apply_rz(a, M_PI / 8.0);
            apply_rz(b, M_PI / 8.0);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(c, a);
            apply_rz(a, -M_PI / 8.0);
            apply_cnot(c, a);
            apply_cnot(a, b);
            apply_rz(b, -M_PI / 8.0);
            apply_cnot(c, b);
            apply_rz(b, M_PI / 8.0);
            apply_cnot(a, b);
            apply_rz(b, -M_PI / 8.0);
            apply_cnot(c, b);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(a, t);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(c, t);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(a, t);
            apply_rz(t, M_PI / 8.0);
            apply_cnot(b, t);
            apply_rz(t, -M_PI / 8.0);
            apply_cnot(c, t);
            apply_h(t);
        }

        // ─── 噪声通道注入（@[noise]/@[coherence] 元数据 → 运行时）──────────
        // channel: 0=depolarizing, 1=dephasing(phase_flip), 2=amplitude_damping, 3=bit_flip
        // param：噪声强度（depolarizing/dephasing/bit_flip 为概率 p，amplitude_damping 为 γ）。
        // 默认空实现；维护态矢量的后端（QVM 等）应重写为对应的噪声通道。
        virtual void apply_noise(size_t qubit_id, int channel, double param)
        {
            (void)qubit_id;
            (void)channel;
            (void)param;
        }

        virtual void apply_qft(size_t lo, size_t hi)
        {
            for (int i = static_cast<int>(hi); i >= static_cast<int>(lo); --i)
            {
                apply_h(static_cast<size_t>(i));
                for (int j = static_cast<int>(lo); j < i; ++j)
                {
                    double theta = 2.0 * M_PI / std::pow(2.0, i - j + 1);
                    apply_crz(static_cast<size_t>(i), static_cast<size_t>(j), theta);
                }
            }
        }

        // 逆 QFT（iQFT）：QFT 的门序反转 + 全部角度取负，严格满足 iqft = qft†。
        virtual void apply_iqft(size_t lo, size_t hi)
        {
            for (int i = static_cast<int>(lo); i <= static_cast<int>(hi); ++i)
            {
                for (int j = i - 1; j >= static_cast<int>(lo); --j)
                {
                    double theta = 2.0 * M_PI / std::pow(2.0, i - j + 1);
                    apply_crz(static_cast<size_t>(i), static_cast<size_t>(j), -theta);
                }
                apply_h(static_cast<size_t>(i));
            }
        }

        // 受控 QFT（cqft）：整个 QFT 受 control 导引。
        //   H(i) → ch(control, i)
        //   CRz(i,j,θ) → ccrz(control,i,j,θ)
        //     = Rz(j,θ/2)·Toffoli(control,i,j)·Rz(j,-θ/2)·Toffoli(control,i,j)·CRz(control,i,θ/2)
        //   （末项补回 Toffoli 路径相对 CNOT 路径缺失的控制相位，使结果严格等于 C[CRz]）
        virtual void apply_cqft(size_t control, size_t lo, size_t hi)
        {
            for (int i = static_cast<int>(hi); i >= static_cast<int>(lo); --i)
            {
                apply_ch(control, static_cast<size_t>(i));
                for (int j = static_cast<int>(lo); j < i; ++j)
                {
                    double theta = 2.0 * M_PI / std::pow(2.0, i - j + 1);
                    apply_rz(static_cast<size_t>(j), theta / 2.0);
                    apply_toffoli(control, static_cast<size_t>(i), static_cast<size_t>(j));
                    apply_rz(static_cast<size_t>(j), -theta / 2.0);
                    apply_toffoli(control, static_cast<size_t>(i), static_cast<size_t>(j));
                    apply_crz(control, static_cast<size_t>(i), theta / 2.0);
                }
            }
        }

        virtual int measure_basis(size_t qubit_id, char basis)
        {
            if (basis == 'X' || basis == 'x')
                apply_h(qubit_id);
            else if (basis == 'Y' || basis == 'y')
            {
                apply_rz(qubit_id, M_PI / 2.0);
                apply_h(qubit_id);
            }
            return measure(qubit_id);
        }

        // 任意基构建：把 |0⟩ 旋转到布洛赫方向 (θ, φ) 的 + 本征态
        //   |b₀⟩ = cos(θ/2)|0⟩ + e^{iφ} sin(θ/2)|1⟩
        // θ ∈ [0, π] 为极角，φ ∈ [0, 2π) 为方位角。
        // 标准基特例：Z 基 = (0,0)；X 基 = (π/2,0)；Y 基 = (π/2,π/2)。
        //
        // 默认实现用门分解（差一个全局相位，物理可观测等价）：
        //   |b₀⟩ = Rz(φ)·Ry(θ)|0⟩，Ry(θ) = Rz(π/2)·H·Rz(θ)·H·Rz(-π/2)。
        // 维护态矢量的后端（如 QVM）应重写为精确的 2×2 酉矩阵实现。
        virtual void apply_basis(size_t qubit_id, double theta, double phi)
        {
            apply_rz(qubit_id, -M_PI / 2.0);
            apply_h(qubit_id);
            apply_rz(qubit_id, theta);
            apply_h(qubit_id);
            apply_rz(qubit_id, M_PI / 2.0);
            apply_rz(qubit_id, phi);
        }

        virtual void apply_braid(size_t a, size_t b)
        {
            apply_swap(a, b);
        }

        // 受控 braid（c_braid）：默认 = 受控 swap（Fredkin），与 braid 默认 = swap 一致。
        // 重写 apply_braid 的后端（如 QVM，braid = e^{iπ/4·X⊗Z}）必须同步重写本函数
        // 为对应的受控版本，否则 steer(braid) 语义与 braid 不一致。
        virtual void apply_cbraid(size_t control, size_t a, size_t b)
        {
            apply_cswap(control, a, b);
        }

        // 读取当前态向量（计算基振幅）与量子比特数。
        // 用于可视化服务（qvm_visualizer）实时反映 rvdl 代码的执行状态。
        // 默认返回空；维护态矢量的后端（如 QVM）应重写。
        virtual std::vector<std::complex<double>> get_state_vector() const { return {}; }
        virtual size_t get_num_qubits() const { return 0; }

        // 后端可表示的最大 qubit 数（自适应感知分配依据）。
        // 真实 QM 芯片返回物理 qubit 数；QVM 按可用内存/压缩策略返回。
        // 返回 0 表示未知/无上限（调用方应保守处理）。
        virtual size_t get_max_qubits() const { return 0; }
    };
}