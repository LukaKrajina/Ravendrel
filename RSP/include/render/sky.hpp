#pragma once

// ============================================================================
// sky —— 大气层模拟（瑞利 / 米氏散射）
// ----------------------------------------------------------------------------
// 单次散射解析近似：根据视线方向与太阳方向计算天空颜色。
//  - 瑞利散射（Rayleigh）：短波长（蓝光）散射更强，产生蓝色天空
//  - 米氏散射（Mie）：气溶胶散射，产生太阳周围的光晕与地平线白化
// 供 CPU 预览（UI 面板）与视口背景色 / 天空着色器共用。
// ============================================================================

#include <cmath>
#include <algorithm>
#include "qpc/math.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace rvdlrsp::render
{

    // 大气参数（可调）
    struct SkyParams
    {
        qpc::Vec3 rayleigh{5.5, 13.0, 22.4};   // 瑞利散射系数（R/G/B，蓝色最强）
        double mie = 21.0;                     // 米氏散射系数
        qpc::Vec3 sun_dir{0.35, 0.75, 0.45};   // 太阳方向（归一化）
        double sun_intensity = 22.0;           // 太阳强度
        double rayleigh_scale = 8.0;           // 瑞利尺度高度（km）
        double mie_scale = 1.2;                // 米氏尺度高度（km）
        double mie_g = 0.76;                   // 米氏各向异性（前向散射）
        double ground_altitude = 0.0;          // 观察点高度（km）
    };

    // 瑞利相位函数
    inline double rayleigh_phase(double cos_theta)
    {
        return 0.75 * (1.0 + cos_theta * cos_theta);
    }

    // 米氏相位函数（Henyey-Greenstein）
    inline double mie_phase(double cos_theta, double g)
    {
        double g2 = g * g;
        double denom = 1.0 + g2 - 2.0 * g * cos_theta;
        denom = std::max(denom, 1e-6);
        return (3.0 / (8.0 * M_PI)) * ((1.0 - g2) * (1.0 + cos_theta * cos_theta)) /
               (denom * std::sqrt(denom));
    }

    // 光学深度近似（沿视线方向，随高度指数衰减）
    inline double optical_depth(double altitude, double cos_view, double scale_height)
    {
        // 简化的 Chapman 函数近似：大气密度随高度指数衰减
        double h = std::max(altitude, 0.0);
        double x = h / scale_height;
        double exp_term = std::exp(-x);
        // 视线上行/下行的近似积分
        double depth = exp_term * scale_height;
        if (cos_view > 0.05)
            depth *= (1.0 / cos_view);
        else
            depth *= 8.0;   // 接近地平线时深度增大
        return depth;
    }

    // 计算天空颜色（视线方向 ray_dir，单位向量）
    inline qpc::Vec3 compute_sky_color(const qpc::Vec3 &ray_dir, const SkyParams &p)
    {
        qpc::Vec3 rd = ray_dir.normalized();
        qpc::Vec3 sd = p.sun_dir.normalized();

        // 视线与太阳夹角余弦
        double cos_theta = std::max(-1.0, std::min(1.0, rd.dot(sd)));
        // 视线高度（向上为正）
        double cos_view = std::max(-1.0, std::min(1.0, rd.y));

        double pr = rayleigh_phase(cos_theta);
        double pm = mie_phase(cos_theta, p.mie_g);

        // 瑞利/米氏光学深度
        double dr = optical_depth(p.ground_altitude, cos_view, p.rayleigh_scale);
        double dm = optical_depth(p.ground_altitude, cos_view, p.mie_scale);

        // 散射强度 = 系数 × 相位 × 深度 × 太阳强度
        double sun_boost = std::max(0.0, 0.15 + sd.y);   // 太阳越高越亮
        double I = p.sun_intensity * sun_boost;

        double r = p.rayleigh.x * pr * dr + p.mie * pm * dm;
        double g = p.rayleigh.y * pr * dr + p.mie * pm * dm;
        double b = p.rayleigh.z * pr * dr + p.mie * pm * dm;

        // 归一化到 [0,1]，用 log 软压缩
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
