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

    class MotorInterface
    {
    public:
        virtual ~MotorInterface() = default;
        virtual void set_position(double pos) = 0;
        virtual void set_velocity(double vel) = 0;
        virtual void set_torque(double torque) = 0;
        virtual const char *name() const = 0;
    };

    class SensorInterface
    {
    public:
        virtual ~SensorInterface() = default;
        virtual std::vector<double> read_values() = 0;
        virtual const char *name() const = 0;
    };

    class UnifiedDevice : public HardwareInterface, public MotorInterface, public SensorInterface
    {
    private:
        std::string name_;
        double position_ = 0.0, velocity_ = 0.0, torque_ = 0.0;

        double encoder_ticks_ = 0.0;
        double encoder_velocity_ = 0.0;
        double ticks_per_revolution_ = 4096.0;
        std::mt19937 rng_{12345u};

    public:
        explicit UnifiedDevice(std::string name, double ticks_per_rev = 4096.0)
            : name_(std::move(name)), ticks_per_revolution_(ticks_per_rev) {}
        const char *name() const override { return name_.c_str(); }

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

        void set_position(double p) override { position_ = p; }
        void set_velocity(double v) override { velocity_ = v; }
        void set_torque(double t) override { torque_ = t; }

        std::vector<double> read_values() override
        {
            return {encoder_ticks_, encoder_velocity_, torque_};
        }

        double get_encoder_ticks() const { return encoder_ticks_; }
        double get_encoder_velocity() const { return encoder_velocity_; }
        double get_ticks_per_revolution() const { return ticks_per_revolution_; }
    };
}