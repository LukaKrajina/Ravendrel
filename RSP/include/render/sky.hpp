#pragma once


#include <cmath>
#include <algorithm>
#include "qpc/math.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace rvdlrsp::render
{

    struct SkyParams
    {
        qpc::Vec3 rayleigh{5.5, 13.0, 22.4};
        double mie = 21.0;
        qpc::Vec3 sun_dir{0.35, 0.75, 0.45};
        double sun_intensity = 22.0;
        double rayleigh_scale = 8.0;
        double mie_scale = 1.2;
        double mie_g = 0.76;
        double ground_altitude = 0.0;
    };

    inline double rayleigh_phase(double cos_theta)
    {
        return 0.75 * (1.0 + cos_theta * cos_theta);
    }

    inline double mie_phase(double cos_theta, double g)
    {
        double g2 = g * g;
        double denom = 1.0 + g2 - 2.0 * g * cos_theta;
        denom = std::max(denom, 1e-6);
        return (3.0 / (8.0 * M_PI)) * ((1.0 - g2) * (1.0 + cos_theta * cos_theta)) /
               (denom * std::sqrt(denom));
    }

    inline double optical_depth(double altitude, double cos_view, double scale_height)
    {
        double h = std::max(altitude, 0.0);
        double x = h / scale_height;
        double exp_term = std::exp(-x);
        double depth = exp_term * scale_height;
        if (cos_view > 0.05)
            depth *= (1.0 / cos_view);
        else
            depth *= 8.0;
        return depth;
    }

    inline qpc::Vec3 compute_sky_color(const qpc::Vec3 &ray_dir, const SkyParams &p)
    {
        qpc::Vec3 rd = ray_dir.normalized();
        qpc::Vec3 sd = p.sun_dir.normalized();

        double cos_theta = std::max(-1.0, std::min(1.0, rd.dot(sd)));
        double cos_view = std::max(-1.0, std::min(1.0, rd.y));

        double pr = rayleigh_phase(cos_theta);
        double pm = mie_phase(cos_theta, p.mie_g);

        double dr = optical_depth(p.ground_altitude, cos_view, p.rayleigh_scale);
        double dm = optical_depth(p.ground_altitude, cos_view, p.mie_scale);

        double sun_boost = std::max(0.0, 0.15 + sd.y);
        double I = p.sun_intensity * sun_boost;

        double r = p.rayleigh.x * pr * dr + p.mie * pm * dm;
        double g = p.rayleigh.y * pr * dr + p.mie * pm * dm;
        double b = p.rayleigh.z * pr * dr + p.mie * pm * dm;

        auto tonemap = [](double v)
        {
            return 1.0 - std::exp(-v * 0.0015 * 22.0);
        };

        return {
            std::min(1.0, tonemap(r * I)),
            std::min(1.0, tonemap(g * I)),
            std::min(1.0, tonemap(b * I))};
    }
}