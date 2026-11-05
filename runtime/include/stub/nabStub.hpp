#pragma once

#include <vector>
#include <map>
#include <cstddef>
#include <cmath>
#include <iostream>

// 量子硬件分发接口（SLM / AOD 光镊）。
//
// 设计要点（本次修正）：
//   原先 Dispatch 的默认实现就是「软件仿真」——把下发内容记录到可查询状态并打印。
//   这会让未接入真实硬件的后端**静默假装已下发**，调用方无从分辨「已送达硬件」
//   与「仅被记录」。现在拆成两层：
//
//     1) IDispatch（纯虚接口）—— 只定义契约。任何后端都必须提供真实驱动实现，
//        未实现即编译期报错，从机制上杜绝「假装下发」。
//     2) SoftwareDispatch（仿真实现）—— 仅供单元测试 / 离线验证，需显式选用；
//        保留 last_slm_frame / tweezer_depths 供断言。
//
//   已有后端（如 NeutralAtomBackend）override 了这两个方法以提供真实驱动，
//   改为纯虚后不受影响。

namespace Stub {

    // ─── 硬件分发契约（纯虚：无默认仿真兜底）─────────────────────────────
    class IDispatch {
    public:
        virtual ~IDispatch() = default;

        // 将相位掩码下发到 SLM 硬件。实现必须真正送达硬件驱动；
        // 相位约定在 [0, 2π)（硬件帧缓冲会 wrap）。
        virtual void dispatch_buffer_to_slm_hardware(
            const std::vector<std::vector<double>>& phase_mask) = 0;

        // 增大指定 qubit 的光镊阱深（AOD 驱动）。
        virtual void increase_tweezer_depth(size_t qubit_id) = 0;
    };

    // 兼容旧名：Dispatch 现指向纯虚接口（原先是带仿真默认实现的基类）。
    using Dispatch = IDispatch;

    // ─── 软件仿真实现（测试 / 离线验证专用，非生产路径）───────────────────
    class SoftwareDispatch : public IDispatch {
    public:
        // 仿真状态（可被单元测试查询，验证下发内容）。
        //   last_slm_frame[y][x] = 像素相位（弧度，[0, 2π)）
        //   tweezer_depths[id]    = 该 qubit 光镊阱深（累加）
        std::vector<std::vector<double>> last_slm_frame;
        std::map<size_t, double> tweezer_depths;

        void dispatch_buffer_to_slm_hardware(
            const std::vector<std::vector<double>>& phase_mask) override {
            last_slm_frame = phase_mask;

            // 相位范围校验：超出 [0, 2π) 的像素计数并告警（硬件帧缓冲会 wrap）。
            constexpr double kTwoPi = 6.283185307179586;
            size_t out_of_range = 0;
            for (const auto& row : phase_mask)
                for (double p : row)
                    if (!(p >= 0.0 && p < kTwoPi))
                        ++out_of_range;

            const size_t h = phase_mask.size();
            const size_t w = phase_mask.empty() ? 0 : phase_mask[0].size();
            std::cout << "[SoftwareDispatch] SLM frame captured "
                      << "(SIMULATION - not real hardware): " << w << "x" << h;
            if (out_of_range)
                std::cerr << "  [warn] " << out_of_range << " pixel(s) phase out of [0, 2pi)";
            std::cout << "\n";
        }

        void increase_tweezer_depth(size_t qubit_id) override {
            tweezer_depths[qubit_id] += 1.0;
        }
    };

}
