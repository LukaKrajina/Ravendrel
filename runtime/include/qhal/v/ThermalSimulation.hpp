#pragma once

#include <algorithm>
#include <cmath>
#include <random>

namespace qhal
{

    class HostThermalProbe
    {
      public:
        struct Reading
        {
            double cpu_c = 0.0;
            double gpu_c = 0.0;
            bool synthetic = true;
        };

        static Reading sample();

      private:
        static Reading sample_linux();
        static Reading sample_windows();
        static Reading synthetic();
    };

    class QVMThermalModel
    {
      private:
        double ambient_c = 25.0;
        double tau_s = 12.0;
        double k_load = 0.9;
        double w_cpu = 0.6, w_gpu = 0.4;
        double t_c = ambient_c;
        std::mt19937 rng{42u};

        double steady_state(double load_c) const
        {
            return ambient_c + k_load * std::max(0.0, load_c - ambient_c);
        }

      public:
        double step(double dt_seconds)
        {
            HostThermalProbe::Reading r = HostThermalProbe::sample();
            double load = w_cpu * r.cpu_c + w_gpu * r.gpu_c;
            double alpha = 1.0 - std::exp(-dt_seconds / tau_s);
            t_c += alpha * (steady_state(load) - t_c);
            std::normal_distribution<double> jitter(0.0, 0.05);
            t_c += jitter(rng);
            return t_c;
        }

        double celsius() const
        {
            return t_c;
        }
        double kelvin() const
        {
            return t_c + 273.15;
        }
    };

    class VirtualThermalController
    {
      private:
        QVMThermalModel model;
        double target_c = 25.0;

      public:
        void update(double dt_seconds = 1.0)
        {
            model.step(dt_seconds);
        }

        double read_celsius() const
        {
            return model.celsius();
        }
        double read_kelvin() const
        {
            return model.kelvin();
        }

        void set_target_c(double t)
        {
            target_c = t;
        }
        double target_celsius() const
        {
            return target_c;
        }
    };
}