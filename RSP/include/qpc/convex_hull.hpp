#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>
#include "math.hpp"

namespace rvdlrsp::qpc
{

    struct ConvexHull
    {
        std::vector<Vec3> vertices;
        std::vector<uint32_t> faces;
        std::vector<Vec3> face_normals;

        Vec3 center() const
        {
            Vec3 c;
            for (const auto &v : vertices)
                c += v;
            return vertices.empty() ? c : c / static_cast<double>(vertices.size());
        }

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

    inline ConvexHull build_convex_hull(std::vector<Vec3> pts)
    {
        ConvexHull out;

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
        if (best < 1e-12)
        {
            out.vertices = P;
            return out;
        }

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
                f.normal = -f.normal;
            f.normal = f.normal.normalized();
            faces.push_back(f);
        };
        add_face(ia, ib, ic);
        add_face(ia, ic, id);
        add_face(ia, id, ib);
        add_face(ib, id, ic);

        for (size_t i = 0; i < P.size(); ++i)
        {
            if (static_cast<int>(i) == ia || static_cast<int>(i) == ib ||
                static_cast<int>(i) == ic || static_cast<int>(i) == id)
                continue;
            const Vec3 &p = P[i];

            std::vector<int> visible;
            for (size_t fi = 0; fi < faces.size(); ++fi)
            {
                if (!faces[fi].alive)
                    continue;
                if (faces[fi].normal.dot(p - P[faces[fi].a]) > 1e-9)
                    visible.push_back(static_cast<int>(fi));
            }
            if (visible.empty())
                continue;

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

            for (int fi : visible)
                faces[fi].alive = false;

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