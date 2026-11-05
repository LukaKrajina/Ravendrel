#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include "mocap.hpp"

namespace rvdlrsp::qcdrc
{

    // ─────────────────────────────────────────────────────────────
    // 运动学逆解(IK)与关节角提取
    //
    // 替换 teleop.hpp 中的 atan2 占位映射,提供:
    //   关节角提取 —— 从人体骨架 3D 关节位置,用向量几何计算
    //      真实的关节弯曲角(肘/肩/腕),而非把位置当角度。
    //   平面 N 连杆臂数值 IK —— 雅可比阻尼最小二乘(DLS),
    //      从末端目标位置反解关节角,支持冗余链与奇异处理。
    // ─────────────────────────────────────────────────────────────

    constexpr double kPi = 3.14159265358979323846;

    // 按名字查找骨架关节(不存在返回 nullptr)
    inline const Joint3D *find_joint(const Skeleton &skel, const std::string &name)
    {
        for (const auto &j : skel.joints)
            if (j.name == name)
                return &j;
        return nullptr;
    }

    // ─── 三点夹角:向量 (a→b) 与 (b→c) 的夹角(弧度,[0,π])──
    inline double joint_angle(const Joint3D &a, const Joint3D &b, const Joint3D &c)
    {
        double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
        double bcx = c.x - b.x, bcy = c.y - b.y, bcz = c.z - b.z;
        double dot = abx * bcx + aby * bcy + abz * bcz;
        double la = std::sqrt(abx * abx + aby * aby + abz * abz);
        double lb = std::sqrt(bcx * bcx + bcy * bcy + bcz * bcz);
        if (la < 1e-12 || lb < 1e-12)
            return 0.0;
        double cosv = dot / (la * lb);
        cosv = std::max(-1.0, std::min(1.0, cosv));
        return std::acos(cosv);
    }

    // ─── 肘弯曲角:伸直 = 0,完全弯曲 → π ─────────────────
    // 上臂(elbow-shoulder)与前臂(wrist-elbow)的夹角:同向伸直=0,
    // 折叠增大至 π。
    inline double elbow_flexion(const Joint3D &shoulder, const Joint3D &elbow,
                                const Joint3D &wrist)
    {
        return joint_angle(shoulder, elbow, wrist);
    }

    // ─── 肩抬升角:上臂相对躯干轴的角度,T-pose = 0,抬臂 > 0 ──
    // 躯干参考向量取 (pelvis - neck)(大致垂直向下)。
    inline double shoulder_lift(const Joint3D &shoulder, const Joint3D &elbow,
                                const Joint3D &neck, const Joint3D &pelvis)
    {
        double ux = elbow.x - shoulder.x, uy = elbow.y - shoulder.y, uz = elbow.z - shoulder.z;
        double tx = pelvis.x - neck.x, ty = pelvis.y - neck.y, tz = pelvis.z - neck.z;
        double dot = ux * tx + uy * ty + uz * tz;
        double lu = std::sqrt(ux * ux + uy * uy + uz * uz);
        double lt = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (lu < 1e-12 || lt < 1e-12)
            return 0.0;
        double c = dot / (lu * lt);
        c = std::max(-1.0, std::min(1.0, c));
        // 上臂与躯干夹角;T-pose 上臂水平 → π/2,抬升角 = π/2 - 夹角
        return kPi * 0.5 - std::acos(c);
    }

    // ─── 平面 N 连杆臂数值 IK(雅可比阻尼最小二乘 DLS)──────
    // 连杆均位于 XY 平面,基座在原点,末端为各连杆首尾相接。
    struct PlanarIKSolver
    {
        struct Result
        {
            std::vector<double> angles; // 解出的关节角(rad)
            bool converged = false;
            int iterations = 0;
            double residual = 0.0; // 末端与目标距离
        };

        // links: 各连杆长度;target: 末端目标 (tx, ty);
        // init: 初始关节角(空则全 0);max_iter/tol 控制迭代。
        static Result solve(const std::vector<double> &links,
                            double tx, double ty,
                            const std::vector<double> &init = {},
                            int max_iter = 200, double tol = 1e-5)
        {
            const size_t n = links.size();
            std::vector<double> q = init.empty() ? std::vector<double>(n, 0.0) : init;
            if (q.size() != n)
                q.assign(n, 0.0);

            Result r;
            if (n == 0)
            {
                r.angles = q;
                r.converged = true;
                return r;
            }

            double lambda = 0.1; // DLS 阻尼(随迭代衰减)

            for (int it = 0; it < max_iter; ++it)
            {
                // 前向运动学 + 累计角
                double theta = 0.0, ex = 0.0, ey = 0.0;
                std::vector<double> csum(n, 0.0);
                for (size_t i = 0; i < n; ++i)
                {
                    theta += q[i];
                    csum[i] = theta;
                    ex += links[i] * std::cos(theta);
                    ey += links[i] * std::sin(theta);
                }

                double exx = tx - ex, eyy = ty - ey;
                double err = std::sqrt(exx * exx + eyy * eyy);
                if (err < tol)
                {
                    r.angles = q;
                    r.converged = true;
                    r.iterations = it;
                    r.residual = err;
                    return r;
                }

                // 雅可比 J (2×n):
                //   Jx_i = -Σ_{j≥i} L_j sin(csum_j)
                //   Jy_i =  Σ_{j≥i} L_j cos(csum_j)
                std::vector<double> Jx(n, 0.0), Jy(n, 0.0);
                for (size_t i = 0; i < n; ++i)
                {
                    double sx = 0.0, sy = 0.0;
                    for (size_t j = i; j < n; ++j)
                    {
                        sx += links[j] * std::sin(csum[j]);
                        sy += links[j] * std::cos(csum[j]);
                    }
                    Jx[i] = -sx;
                    Jy[i] = sy;
                }

                // J J^T (2×2)
                double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    a += Jx[i] * Jx[i];
                    b += Jx[i] * Jy[i];
                    c += Jy[i] * Jx[i];
                    d += Jy[i] * Jy[i];
                }
                // 阻尼正则
                double la2 = lambda * lambda;
                a += la2;
                d += la2;

                double det = a * d - b * c;
                if (std::fabs(det) < 1e-12)
                    break; // 奇异,停止迭代

                double inv_a = d / det, inv_b = -b / det;
                double inv_c = -c / det, inv_d = a / det;

                // g = (J J^T + λ²I)^-1 · e
                double gx = inv_a * exx + inv_b * eyy;
                double gy = inv_c * exx + inv_d * eyy;

                // dq = J^T · g
                double max_dq = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    double dq = Jx[i] * gx + Jy[i] * gy;
                    q[i] += dq;
                    max_dq = std::max(max_dq, std::fabs(dq));
                }

                lambda = std::max(1e-4, lambda * 0.9);
                if (max_dq < tol)
                {
                    r.angles = q;
                    r.converged = err < tol;
                    r.iterations = it;
                    r.residual = err;
                    return r;
                }
            }

            // 未收敛:返回当前解与最终残差
            double theta = 0.0, ex = 0.0, ey = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                theta += q[i];
                ex += links[i] * std::cos(theta);
                ey += links[i] * std::sin(theta);
            }
            r.angles = q;
            r.iterations = max_iter;
            r.residual = std::sqrt((tx - ex) * (tx - ex) + (ty - ey) * (ty - ey));
            r.converged = r.residual < tol;
            return r;
        }
    };
}