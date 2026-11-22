#pragma once
#include <string>
#include <mutex>
#include <memory>
#include <cmath>
#include <iostream>
#include <random>
#include "observability.hpp"

namespace rvdlrsp::hardware
{


    class IActuator
    {
    public:
        virtual ~IActuator() = default;
        virtual void set_torque(double torque) = 0;
        virtual void set_velocity(double velocity) = 0;
        virtual void set_position(double position) = 0;
        virtual bool enable() = 0;
        virtual bool disable() = 0;
        virtual bool emergency_stop() = 0;
        virtual std::string name() const = 0;
    };

    class IEncoder
    {
    public:
        virtual ~IEncoder() = default;
        virtual double read_position() = 0;
        virtual double read_velocity() = 0;
    };

    class SimActuator : public IActuator, public IEncoder
    {
    private:
        std::string name_;
        double position_ = 0.0;
        double velocity_ = 0.0;
        double torque_ = 0.0;
        double command_position_ = 0.0;
        bool enabled_ = true;
        mutable std::mt19937 rng_{std::random_device{}()};

    public:
        explicit SimActuator(std::string name = "SimActuator") : name_(std::move(name))
        {
            RVDLRSP_INFO("hw") << "Sim actuator online ('" << name_ << "').";
        }

        void set_torque(double torque) override { torque_ = torque; }
        void set_velocity(double velocity) override { velocity_ = velocity; }

        void set_position(double position) override
        {
            command_position_ = position;
            if (enabled_)
                position_ += (command_position_ - position_) * 0.2;
        }

        bool enable() override { enabled_ = true; return true; }
        bool disable() override { enabled_ = false; return true; }
        bool emergency_stop() override
        {
            enabled_ = false;
            torque_ = 0.0;
            velocity_ = 0.0;
            RVDLRSP_WARN("hw") << "'" << name_ << "' emergency stop.";
            return true;
        }

        std::string name() const override { return name_; }

        double read_position() override { return position_; }
        double read_velocity() override { return velocity_; }
    };

    class ExternalActuator : public IActuator, public IEncoder
    {
    private:
        std::string name_;
        double position_ = 0.0;
        double velocity_ = 0.0;
        bool enabled_ = true;
        mutable std::mutex mtx_;
        double torque_cmd_ = 0.0;
        double velocity_cmd_ = 0.0;
        double position_cmd_ = 0.0;
        bool has_pending_ = false;

    public:
        explicit ExternalActuator(std::string name = "ExternalActuator")
            : name_(std::move(name))
        {
            RVDLRSP_INFO("hw") << "External actuator online ('" << name_ << "').";
        }

        void set_torque(double torque) override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            torque_cmd_ = torque;
            has_pending_ = true;
        }
        void set_velocity(double velocity) override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            velocity_cmd_ = velocity;
            has_pending_ = true;
        }
        void set_position(double position) override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            position_cmd_ = position;
            has_pending_ = true;
        }

        bool drain_commands(double &torque, double &velocity, double &position)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!has_pending_)
                return false;
            torque = torque_cmd_;
            velocity = velocity_cmd_;
            position = position_cmd_;
            has_pending_ = false;
            return true;
        }

        bool enable() override { enabled_ = true; return true; }
        bool disable() override { enabled_ = false; return true; }
        bool emergency_stop() override
        {
            enabled_ = false;
            RVDLRSP_WARN("hw") << "'" << name_ << "' emergency stop.";
            return true;
        }

        std::string name() const override { return name_; }

        void push_encoder(double position, double velocity)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            position_ = position;
            velocity_ = velocity;
        }

        double read_position() override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return position_;
        }
        double read_velocity() override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return velocity_;
        }
    };

    inline std::shared_ptr<IActuator> make_actuator(const std::string &name,
                                                    bool real = false)
    {
        if (real)
            return std::make_shared<ExternalActuator>(name);
        return std::make_shared<SimActuator>(name);
    }
}