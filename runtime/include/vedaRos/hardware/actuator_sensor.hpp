#pragma once
#include <string>
#include <vector>
#include <utility>
#include <random>
#include <cmath>
#include <iostream>
#include "hardware_abstraction.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vedaros::hardware
{

    // 统一电机接口
    class MotorInterface
    {
    public:
        virtual ~MotorInterface() = default;
        virtual void set_position(double pos) = 0;
        virtual void set_velocity(double vel) = 0;
        virtual void set_torque(double torque) = 0;
        virtual const char *name() const = 0;
    };

    // 统一传感器接口
    class SensorInterface
    {
    public:
        virtual ~SensorInterface() = default;
        virtual std::vector<double> read_values() = 0;
        virtual const char *name() const = 0;
    };

    // 一体化设备：同时具备电机与传感器，并实现硬件抽象
    class UnifiedDevice : public HardwareInterface, public MotorInterface, public SensorInterface
    {
    private:
        std::string name_;
        // 命令值（由控制律下发）
        double position_ = 0.0, velocity_ = 0.0, torque_ = 0.0;

        // 编码器状态（由 read() 采样得到）
        double encoder_ticks_ = 0.0;           // 累计编码器计数
        double encoder_velocity_ = 0.0;        // 每采样周期的 tick 增量
        double ticks_per_revolution_ = 4096.0; // 每圈脉冲数（增量编码器）
        std::mt19937 rng_{12345u};             // 确定性噪声源

    public:
        explicit UnifiedDevice(std::string name, double ticks_per_rev = 4096.0)
            : name_(std::move(name)), ticks_per_revolution_(ticks_per_rev) {}
        const char *name() const override { return name_.c_str(); }

        // 硬件接口：编码器采样
        //
        // 将命令位置（弧度）转换为编码器计数，叠加 ±0.5 tick 的量化
        // 噪声与小幅高斯抖动，模拟真实增量编码器的读数；速度由相邻
        // 两次采样间的 tick 差分估计。
        void read() override
        {
            double raw_ticks = position_ * ticks_per_revolution_ / (2.0 * M_PI);

            std::normal_distribution<double> jitter(0.0, 0.25);
            double sampled_ticks = raw_ticks + jitter(rng_);

            encoder_velocity_ = sampled_ticks - encoder_ticks_;
            encoder_ticks_ = sampled_ticks;
        }

        void write() override
        {
            std::cout << "[vedaRos.hw] '" << name_ << "' -> pos=" << position_
                      << ", vel=" << velocity_ << ", torque=" << torque_ << "\n";
        }

        // 电机接口
        void set_position(double p) override { position_ = p; }
        void set_velocity(double v) override { velocity_ = v; }
        void set_torque(double t) override { torque_ = t; }

        // 传感器接口：返回编码器采样值（ticks、每周期增量、力矩）
        std::vector<double> read_values() override
        {
            return {encoder_ticks_, encoder_velocity_, torque_};
        }

        // 编码器访问器
        double get_encoder_ticks() const { return encoder_ticks_; }
        double get_encoder_velocity() const { return encoder_velocity_; }
        double get_ticks_per_revolution() const { return ticks_per_revolution_; }
    };
}