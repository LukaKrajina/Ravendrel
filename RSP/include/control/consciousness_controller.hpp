#pragma once
#include <vector>
#include <cstdint>
#include <iostream>

namespace rvdlrsp::control
{

    // 脑意识调制器：作用于 PD 控制 / 遥操作驱动
    struct ConsciousnessModulators
    {
        double gain_scale = 1.0;      // 控制增益缩放（兴奋度高 → 放大）
        double damping_scale = 1.0;   // 阻尼缩放（兴奋度低 → 稳定优先）
        double target_offset_x = 0.0; // 目标位置偏移
        double target_offset_z = 0.0;
        double arousal = 0.5; // 归一化兴奋度 [0,1]
    };

    class ConsciousnessController
    {
    public:
        // 从脑量子波测量结果（0/1 序列）计算调制因子
        // brain_bits：脑量子态测量结果，1 表示高激活
        static ConsciousnessModulators compute(const std::vector<int> &brain_bits)
        {
            ConsciousnessModulators m;
            if (brain_bits.empty())
                return m;

            // 平均兴奋度
            double sum = 0.0;
            for (int b : brain_bits)
                sum += (b != 0) ? 1.0 : 0.0;
            m.arousal = sum / static_cast<double>(brain_bits.size());

            // 兴奋度高 → 增益放大、阻尼降低（激进）；低 → 增益降低、阻尼提高（保守）
            m.gain_scale = 0.7 + 0.6 * m.arousal;    // [0.7, 1.3]
            m.damping_scale = 1.3 - 0.6 * m.arousal; // [0.7, 1.3]

            // 意识驱动的目标偏移（兴奋度偏离中性 0.5 时偏移目标）
            double neutral = m.arousal - 0.5;
            m.target_offset_x = neutral * 2.0; // [-1.0, 1.0]
            m.target_offset_z = -neutral * 1.5;

            return m;
        }

        // 输入原始 PD 增益/阻尼，输出调制后的值
        static double apply_gain(double base_kp, double gain_scale)
        {
            return base_kp * gain_scale;
        }

        static double apply_damping(double base_kd, double damping_scale)
        {
            return base_kd * damping_scale;
        }
    };
}