#pragma once
#include "TachyonField.hpp"
#include "Integrators.hpp"
#include "EnergyMonitor.hpp"

namespace rvdl::spacetime
{
    // 叶状结构（Foliation）
    // 把 n 维空间视为切片（拓扑曲面）的堆叠：时间离散为一叠空间切片，
    // 每一片是一个 Grid（T^N 环面），用辛积分在相邻切片之间推进 φ、π。
    class Foliation
    {
    public:
        explicit Foliation(const TachyonConfig &cfg) : field_(cfg) {}

        // 推进一步（从第 n 片到第 n+1 片）
        void step()
        {
            symplectic_step(field_, field_.cfg.dt, field_.cfg.integrator);
            ++step_;
        }

        // 当前哈密顿量（总能量）
        double energy() const
        {
            return energy_snapshot().total;
        }

        EnergySnapshot energy_snapshot() const
        {
            return measure_energy(field_.grid, field_.phi, field_.pi,
                                  field_.cfg.mu2, field_.cfg.lambda,
                                  field_.cfg.diff_order);
        }

        TachyonField &field() { return field_; }
        const TachyonField &field() const { return field_; }
        int step_count() const { return step_; }

    private:
        TachyonField field_;
        int step_ = 0;
    };
}