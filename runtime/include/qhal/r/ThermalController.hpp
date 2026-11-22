#pragma once

#include <iostream>
#include <memory>
#include <random>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace qhal
{

    enum class TemperatureUnit
    {
        Kelvin,
        Millikelvin,
        Celsius
    };

    struct Temperature
    {
        double kelvin = 0.0;

        double to(TemperatureUnit unit) const
        {
            if (unit == TemperatureUnit::Celsius)
                return kelvin - 273.15;
            if (unit == TemperatureUnit::Millikelvin)
                return kelvin * 1000.0;
            return kelvin;
        }
    };

    struct ThermalSetpoint
    {
        static constexpr double SuperconductingMixChamber_K = 0.015;
        static constexpr double TrappedIonChamber_K = 300.0;
        static constexpr double NeutralAtomChamber_K = 300.0;
        static constexpr double PhotonicChip_K =
            300.0;
        static constexpr double CsVaporCell_K = 303.15;
    };

    class ITemperatureSensor
    {
      public:
        virtual ~ITemperatureSensor() = default;
        virtual double read_kelvin() = 0;
        virtual const char* name() const = 0;







        virtual bool simulated() const
        {
            return false;
        }
    };

    class CryostatThermometer : public ITemperatureSensor
    {
      private:
        double baseline_k;
        double noise_k;
        double drift_std_k;
        double drift_state_k = 0.0;
        std::mt19937 rng{12345u};

      public:
        explicit CryostatThermometer(double baseline_kelvin, double noise_kelvin = 0.0002,
                                     double drift_kelvin = 0.00005)
            : baseline_k(baseline_kelvin), noise_k(noise_kelvin), drift_std_k(drift_kelvin)
        {
        }

        const char* name() const override
        {
            return "RuO2_MXC";
        }

        bool simulated() const override
        {
            return true;
        }

        double read_kelvin() override
        {
            std::normal_distribution<double> white(0.0, noise_k);
            std::normal_distribution<double> drift_step(0.0, drift_std_k);
            drift_state_k = 0.995 * drift_state_k + drift_step(rng);
            return baseline_k + drift_state_k + white(rng);
        }
    };

    class ISerialPort
    {
      public:
        virtual ~ISerialPort() = default;
        virtual bool open(const std::string& device) = 0;
        virtual void close() = 0;
        virtual bool is_open() const = 0;
        virtual int write(const std::string& data) = 0;
        virtual std::string read_line(double timeout_ms = 1000.0) = 0;
    };

    class SerialCommandSensor : public ITemperatureSensor
    {
      protected:
        std::unique_ptr<ISerialPort> port;
        std::string device;
        int gpib_address = 0;

      public:
        SerialCommandSensor(std::unique_ptr<ISerialPort> p, const std::string& device_path,
                            int gpib_addr = 0);
        ~SerialCommandSensor() override;

        bool connect();
        void disconnect();
        bool is_connected() const;
    };

    class Lakeshore336Sensor : public SerialCommandSensor
    {
      private:
        char channel = 'A';

      public:
        Lakeshore336Sensor(std::unique_ptr<ISerialPort> p, const std::string& device_path,
                           char input_channel = 'A', int gpib_addr = 0);

        const char* name() const override;
        double read_kelvin() override;
    };

    class OxfordMercurySensor : public SerialCommandSensor
    {
      private:
        std::string read_cmd;

      public:
        OxfordMercurySensor(std::unique_ptr<ISerialPort> p, const std::string& device_path,
                            const std::string& temp_read_cmd, int gpib_addr = 0);

        const char* name() const override;
        double read_kelvin() override;
    };

    std::unique_ptr<Lakeshore336Sensor>
    make_lakeshore336_sensor(const std::string& device_path, char channel = 'A', int gpib_addr = 0);

    std::unique_ptr<OxfordMercurySensor>
    make_oxford_mercuryitc_sensor(const std::string& device_path, int thermometer_id = 1,
                                  int gpib_addr = 0);

    class HeaterPID
    {
      private:
        double kp, ki, kd;
        double integral = 0.0;
        double prev_error = 0.0;

      public:
        HeaterPID(double p, double i, double d) : kp(p), ki(i), kd(d) {}

        double update(double error, double dt_seconds)
        {
            if (dt_seconds <= 0.0)
                dt_seconds = 1.0;
            integral += error * dt_seconds;
            double derivative = (error - prev_error) / dt_seconds;
            prev_error = error;
            return kp * error + ki * integral + kd * derivative;
        }
    };

    class ThermalController
    {
      private:
        std::unique_ptr<ITemperatureSensor> sensor;
        HeaterPID pid;
        double target_k;

      public:
        ThermalController(std::unique_ptr<ITemperatureSensor> s, double target_kelvin)
            : sensor(std::move(s)), pid(1.0, 0.1, 0.05), target_k(target_kelvin)
        {
        }

        double read_kelvin() const
        {
            return sensor->read_kelvin();
        }
        double target_kelvin() const
        {
            return target_k;
        }

        const ITemperatureSensor* temperature_sensor() const
        {
            return sensor.get();
        }

        void set_target_kelvin(double k)
        {
            target_k = k;
        }

        double stabilize(double dt_seconds = 1.0)
        {
            double current = sensor->read_kelvin();
            double heater_power = pid.update(target_k - current, dt_seconds);
            std::cout << "[ThermalController] " << sensor->name() << " T=" << current * 1000.0
                      << " mK"
                      << " (target " << target_k * 1000.0 << " mK)"
                      << " heater=" << heater_power << " W\n";
            return current;
        }
    };
}