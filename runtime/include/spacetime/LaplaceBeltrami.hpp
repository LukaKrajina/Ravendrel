#pragma once
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>

namespace rvdl::spacetime
{

    constexpr double LB_PI = 3.14159265358979323846;

    struct TriMesh
    {
        std::vector<std::array<double, 3>> vertices;
        std::vector<std::array<int, 3>> triangles;

        int vertex_count() const { return static_cast<int>(vertices.size()); }
        int triangle_count() const { return static_cast<int>(triangles.size()); }
        int euler_characteristic() const
        {
            std::vector<std::array<int, 2>> edges;
            auto add_edge = [&](int a, int b)
            {
                int x = std::min(a, b), y = std::max(a, b);
                edges.push_back({x, y});
            };
            for (const auto &t : triangles)
            {
                add_edge(t[0], t[1]);
                add_edge(t[1], t[2]);
                add_edge(t[2], t[0]);
            }
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            int E = static_cast<int>(edges.size());
            return vertex_count() - E + triangle_count();
        }

        int genus() const { return (2 - euler_characteristic()) / 2; }
    };

    inline void compact_vertices(TriMesh &m)
    {
        std::vector<int> used(m.vertex_count(), 0);
        for (const auto &t : m.triangles)
            for (int k = 0; k < 3; ++k)
                used[t[k]] = 1;

        std::vector<int> remap(m.vertex_count(), -1);
        std::vector<std::array<double, 3>> new_verts;
        int cnt = 0;
        for (int i = 0; i < m.vertex_count(); ++i)
            if (used[i])
            {
                remap[i] = cnt++;
                new_verts.push_back(m.vertices[i]);
            }
        for (auto &t : m.triangles)
            for (int k = 0; k < 3; ++k)
                t[k] = remap[t[k]];
        m.vertices = std::move(new_verts);
    }
    
    inline double triangle_area(const TriMesh &m, int t)
    {
        const auto &a = m.vertices[m.triangles[t][0]];
        const auto &b = m.vertices[m.triangles[t][1]];
        const auto &c = m.vertices[m.triangles[t][2]];
        double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        return 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }

    inline double dist2(const TriMesh &m, int a, int b)
    {
        double dx = m.vertices[a][0] - m.vertices[b][0];
        double dy = m.vertices[a][1] - m.vertices[b][1];
        double dz = m.vertices[a][2] - m.vertices[b][2];
        return dx * dx + dy * dy + dz * dz;
    }

    inline void apply_laplace_beltrami(const TriMesh &m,
                                       const std::vector<double> &f,
                                       std::vector<double> &lap)
    {
        const int V = m.vertex_count();
        lap.assign(V, 0.0);
        std::vector<double> area(V, 0.0);
        std::vector<double> bary(V, 0.0);
        std::vector<double> acc(V, 0.0);

        auto edge_cotan = [&](int vi, int vj, int third) -> double
        {
            const auto &A = m.vertices[vi];
            const auto &B = m.vertices[vj];
            const auto &C = m.vertices[third];
            double cax = A[0] - C[0], cay = A[1] - C[1], caz = A[2] - C[2];
            double cbx = B[0] - C[0], cby = B[1] - C[1], cbz = B[2] - C[2];
            double dot = cax * cbx + cay * cby + caz * cbz;
            double cx = cay * cbz - caz * cby, cy = caz * cbx - cax * cbz, cz = cax * cby - cay * cbx;
            double cross = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (cross < 1e-14)
                return 0.0;
            return dot / cross;
        };

        for (int t = 0; t < m.triangle_count(); ++t)
        {
            int i0 = m.triangles[t][0], i1 = m.triangles[t][1], i2 = m.triangles[t][2];
            double c0 = edge_cotan(i1, i2, i0);
            double c1 = edge_cotan(i2, i0, i1);
            double c2 = edge_cotan(i0, i1, i2);
            double e01 = dist2(m, i0, i1), e02 = dist2(m, i0, i2), e12 = dist2(m, i1, i2);
            area[i0] += 0.125 * (e01 * c2 + e02 * c1);
            area[i1] += 0.125 * (e12 * c0 + e01 * c2);
            area[i2] += 0.125 * (e02 * c1 + e12 * c0);
            double a3 = triangle_area(m, t) / 3.0;
            bary[i0] += a3;
            bary[i1] += a3;
            bary[i2] += a3;
            acc[i0] += 0.5 * (c2 * (f[i1] - f[i0]) + c1 * (f[i2] - f[i0]));
            acc[i1] += 0.5 * (c2 * (f[i0] - f[i1]) + c0 * (f[i2] - f[i1]));
            acc[i2] += 0.5 * (c1 * (f[i0] - f[i2]) + c0 * (f[i1] - f[i2]));
        }

        for (int i = 0; i < V; ++i)
        {
            double A = (area[i] > 1e-14) ? area[i] : bary[i];
            lap[i] = (A > 1e-14) ? acc[i] / A : 0.0;
        }
    }

    // ─── 网格生成器 ─────────────────────────────────────────────
    inline TriMesh make_sphere_mesh(double R, int ntheta, int nphi)
    {
        TriMesh m;
        m.vertices.push_back({0.0, 0.0, R});
        for (int i = 1; i < ntheta; ++i)
        {
            double th = LB_PI * i / ntheta;
            double st = std::sin(th), ct = std::cos(th);
            for (int j = 0; j < nphi; ++j)
            {
                double ph = 2.0 * LB_PI * j / nphi;
                m.vertices.push_back({R * st * std::cos(ph), R * st * std::sin(ph), R * ct});
            }
        }
        m.vertices.push_back({0.0, 0.0, -R});

        auto ring = [&](int i) -> int
        { return 1 + (i - 1) * nphi; };

        for (int j = 0; j < nphi; ++j)
            m.triangles.push_back({0, ring(1) + j, ring(1) + (j + 1) % nphi});

        for (int i = 1; i < ntheta - 1; ++i)
            for (int j = 0; j < nphi; ++j)
            {
                int a = ring(i) + j, b = ring(i) + (j + 1) % nphi;
                int c = ring(i + 1) + j, d = ring(i + 1) + (j + 1) % nphi;
                m.triangles.push_back({a, c, b});
                m.triangles.push_back({b, c, d});
            }

        int south = static_cast<int>(m.vertices.size()) - 1;
        for (int j = 0; j < nphi; ++j)
            m.triangles.push_back({ring(ntheta - 1) + j, south, ring(ntheta - 1) + (j + 1) % nphi});

        return m;
    }

    inline TriMesh make_torus_mesh(double R, double r, int n1, int n2)
    {
        TriMesh m;
        for (int i = 0; i < n1; ++i)
        {
            double th = 2.0 * LB_PI * i / n1;
            for (int j = 0; j < n2; ++j)
            {
                double ph = 2.0 * LB_PI * j / n2;
                double rr = R + r * std::cos(ph);
                m.vertices.push_back({rr * std::cos(th), rr * std::sin(th), r * std::sin(ph)});
            }
        }
        for (int i = 0; i < n1; ++i)
            for (int j = 0; j < n2; ++j)
            {
                int a = i * n2 + j;
                int b = ((i + 1) % n1) * n2 + j;
                int c = i * n2 + (j + 1) % n2;
                int d = ((i + 1) % n1) * n2 + (j + 1) % n2;
                m.triangles.push_back({a, c, b});
                m.triangles.push_back({b, c, d});
            }
        return m;
    }

    inline TriMesh make_genus2_mesh(double R, double r, int n1, int n2)
    {
        const int a = 3;
        const int b = 3;
        const double d = R + r;

        TriMesh A = make_torus_mesh(R, r, n1, n2);
        TriMesh B = make_torus_mesh(R, r, n1, n2);
        for (auto &v : A.vertices)
            v[0] -= d;
        for (auto &v : B.vertices)
            v[0] += d;

        auto vid = [n1, n2](int i, int j)
        {
            int ii = ((i % n1) + n1) % n1;
            int jj = ((j % n2) + n2) % n2;
            return ii * n2 + jj;
        };

        const int A_th0 = 0;
        const int B_th0 = n1 / 2;

        auto boundary = [&](int th0)
        {
            std::vector<int> ring;
            for (int i = 0; i < a; ++i)
                ring.push_back(vid(th0 + i, 0));
            for (int j = 0; j < b; ++j)
                ring.push_back(vid(th0 + a, j));
            for (int i = a; i >= 1; --i)
                ring.push_back(vid(th0 + i, b));
            for (int j = b; j >= 1; --j)
                ring.push_back(vid(th0, j));
            return ring;
        };
        std::vector<int> ringA = boundary(A_th0);
        std::vector<int> ringB = boundary(B_th0);

        auto tri_in_hole = [&](int i, int j, int th0)
        {
            return (i >= th0 && i < th0 + a) && (j >= 0 && j < b);
        };

        TriMesh out;
        out.vertices = A.vertices;
        const int A_vc = A.vertex_count();

        for (int i = 0; i < n1; ++i)
            for (int j = 0; j < n2; ++j)
            {
                if (tri_in_hole(i, j, A_th0))
                    continue;
                int v00 = vid(i, j), v01 = vid(i, j + 1), v10 = vid(i + 1, j), v11 = vid(i + 1, j + 1);
                out.triangles.push_back({v00, v01, v10});
                out.triangles.push_back({v01, v11, v10});
            }

        for (const auto &v : B.vertices)
            out.vertices.push_back(v);
        for (int i = 0; i < n1; ++i)
            for (int j = 0; j < n2; ++j)
            {
                if (tri_in_hole(i, j, B_th0))
                    continue;
                int v00 = A_vc + vid(i, j), v01 = A_vc + vid(i, j + 1);
                int v10 = A_vc + vid(i + 1, j), v11 = A_vc + vid(i + 1, j + 1);
                out.triangles.push_back({v00, v01, v10});
                out.triangles.push_back({v01, v11, v10});
            }

        const int mring = static_cast<int>(ringA.size());
        for (int k = 0; k < mring; ++k)
        {
            int kk = (k + 1) % mring;
            int a0 = ringA[k], a1 = ringA[kk];
            int b0 = A_vc + ringB[k], b1 = A_vc + ringB[kk];
            out.triangles.push_back({a0, a1, b1});
            out.triangles.push_back({a0, b1, b0});
        }
        
        compact_vertices(out);
        return out;
    }
}