#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>
#include "math.hpp"

namespace rvdlrsp::qpc
{

    // 凸多面体：顶点 + 三角形面（每 3 个索引一组）+ 单位面法线（朝外）
    struct ConvexHull
    {
        std::vector<Vec3> vertices;
        std::vector<uint32_t> faces;    // 三角形，每 3 个索引一组
        std::vector<Vec3> face_normals; // 每面一个（单位，朝外）

        Vec3 center() const
        {
            Vec3 c;
            for (const auto &v : vertices)
                c += v;
            return vertices.empty() ? c : c / static_cast<double>(vertices.size());
        }

        // 沿方向 d 的最远支撑点
        Vec3 support(const Vec3 &d) const
        {
            Vec3 best = vertices[0];
            double bd = best.dot(d);
            for (const auto &v : vertices)
            {
                double dt = v.dot(d);
                if (dt > bd)
                {
                    bd = dt;
                    best = v;
                }
            }
            return best;
        }
    };

    // 构建期临时面
    struct HullFace
    {
        uint32_t a, b, c;
        Vec3 normal;
        bool alive = true;
    };

    inline bool hull_edge_shared(const HullFace &f, uint32_t x, uint32_t y)
    {
        return (f.a == x && f.b == y) || (f.b == x && f.a == y) ||
               (f.b == x && f.c == y) || (f.c == x && f.b == y) ||
               (f.c == x && f.a == y) || (f.a == x && f.c == y);
    }

    // QuickHull 增量法：从点云生成凸包
    inline ConvexHull build_convex_hull(std::vector<Vec3> pts)
    {
        ConvexHull out;

        // 去重
        std::vector<Vec3> P;
        for (const auto &p : pts)
        {
            bool dup = false;
            for (const auto &q : P)
                if ((p - q).length_sq() < 1e-14)
                {
                    dup = true;
                    break;
                }
            if (!dup)
                P.push_back(p);
        }

        if (P.size() < 4)
        {
            out.vertices = P;
            if (P.size() == 3)
            {
                Vec3 n = (P[1] - P[0]).cross(P[2] - P[0]).normalized();
                out.faces = {0, 1, 2};
                out.face_normals.push_back(n);
            }
            return out;
        }

        // 最远点对（作为四面体的一条边）
        int ia = 0, ib = 0;
        double best = -1;
        for (size_t i = 0; i < P.size(); ++i)
            for (size_t j = i + 1; j < P.size(); ++j)
            {
                double d = (P[i] - P[j]).length_sq();
                if (d > best)
                {
                    best = d;
                    ia = static_cast<int>(i);
                    ib = static_cast<int>(j);
                }
            }

        // 离 ab 线最远的点
        Vec3 AB = P[ib] - P[ia];
        int ic = 0;
        best = -1;
        for (size_t i = 0; i < P.size(); ++i)
        {
            double d = (P[i] - P[ia]).cross(AB).length_sq();
            if (d > best)
            {
                best = d;
                ic = static_cast<int>(i);
            }
        }

        // 离 abc 平面最远的点
        Vec3 n0 = (P[ib] - P[ia]).cross(P[ic] - P[ia]);
        int id = 0;
        best = -1;
        for (size_t i = 0; i < P.size(); ++i)
        {
            double d = std::fabs((P[i] - P[ia]).dot(n0));
            if (d > best)
            {
                best = d;
                id = static_cast<int>(i);
            }
        }
        if (best < 1e-12) // 共面退化
        {
            out.vertices = P;
            return out;
        }

        // 初始四面体
        std::vector<HullFace> faces;
        Vec3 center = (P[ia] + P[ib] + P[ic] + P[id]) * 0.25;
        auto add_face = [&](uint32_t x, uint32_t y, uint32_t z)
        {
            HullFace f;
            f.a = x;
            f.b = y;
            f.c = z;
            f.normal = (P[y] - P[x]).cross(P[z] - P[x]);
            if (f.normal.dot(center - P[x]) > 0)
                f.normal = -f.normal; // 法线朝外
            f.normal = f.normal.normalized();
            faces.push_back(f);
        };
        add_face(ia, ib, ic);
        add_face(ia, ic, id);
        add_face(ia, id, ib);
        add_face(ib, id, ic);

        // 增量添加剩余点
        for (size_t i = 0; i < P.size(); ++i)
        {
            if (static_cast<int>(i) == ia || static_cast<int>(i) == ib ||
                static_cast<int>(i) == ic || static_cast<int>(i) == id)
                continue;
            const Vec3 &p = P[i];

            // 可见面（p 在面外侧）
            std::vector<int> visible;
            for (size_t fi = 0; fi < faces.size(); ++fi)
            {
                if (!faces[fi].alive)
                    continue;
                if (faces[fi].normal.dot(p - P[faces[fi].a]) > 1e-9)
                    visible.push_back(static_cast<int>(fi));
            }
            if (visible.empty())
                continue; // 点在凸包内

            // 收集边界边（只属于一个可见面的边）
            std::vector<std::pair<uint32_t, uint32_t>> horizon;
            for (int fi : visible)
            {
                HullFace &f = faces[fi];
                uint32_t e[3][2] = {{f.a, f.b}, {f.b, f.c}, {f.c, f.a}};
                for (auto &ed : e)
                {
                    bool shared = false;
                    for (int fj : visible)
                    {
                        if (fj == fi)
                            continue;
                        if (hull_edge_shared(faces[fj], ed[0], ed[1]))
                        {
                            shared = true;
                            break;
                        }
                    }
                    if (!shared)
                        horizon.push_back({ed[0], ed[1]});
                }
            }

            // 删除可见面
            for (int fi : visible)
                faces[fi].alive = false;

            // 用 p 与边界边构成新面
            for (auto &ed : horizon)
            {
                HullFace f;
                f.a = ed.first;
                f.b = ed.second;
                f.c = static_cast<uint32_t>(i);
                f.normal = (P[ed.second] - P[ed.first]).cross(p - P[ed.first]);
                if (f.normal.dot(center - P[ed.first]) > 0)
                    f.normal = -f.normal;
                f.normal = f.normal.normalized();
                faces.push_back(f);
            }
        }

        // 收集存活面
        out.vertices = P;
        for (auto &f : faces)
        {
            if (!f.alive)
                continue;
            out.faces.push_back(f.a);
            out.faces.push_back(f.b);
            out.faces.push_back(f.c);
            out.face_normals.push_back(f.normal);
        }
        return out;
    }

    // 把局部凸包变换到世界空间（平移 + 旋转）
    inline ConvexHull transform_hull(const ConvexHull &h, const Vec3 &pos, const Quat &rot)
    {
        ConvexHull w;
        w.vertices.reserve(h.vertices.size());
        for (const auto &v : h.vertices)
            w.vertices.push_back(pos + rot.rotate(v));
        w.faces = h.faces;
        w.face_normals.reserve(h.face_normals.size());
        for (const auto &n : h.face_normals)
            w.face_normals.push_back(rot.rotate(n));
        return w;
    }

    // 点到凸包最近点（面投影近似），返回距离（点在内侧时仍返回最近面距离）
    inline double closest_point_on_convex(const ConvexHull &H, const Vec3 &p, Vec3 &out_p)
    {
        double best = 1e30;
        Vec3 bp = p;
        for (size_t fi = 0; fi < H.face_normals.size(); ++fi)
        {
            uint32_t a = H.faces[fi * 3];
            const Vec3 &n = H.face_normals[fi];
            double d = (p - H.vertices[a]).dot(n);
            double ad = std::fabs(d);
            if (ad < best)
            {
                best = ad;
                bp = p - n * d;
            }
        }
        out_p = bp;
        return best;
    }
}