#pragma once
#include "../components/window.h"
#include "../i18n.hpp"
#include <complex>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <limits.h>

namespace qgui
{
    // 3D 布洛赫球：带 Lambert 光照的实体球面 + 可旋转视角 + 三色坐标轴 + 布洛赫向量。
    // 球面由 UV 球体三角形网格构成，CPU 端做 3D 旋转、面法线光照与画家算法深度排序，
    // 三角形经 nk_fill_triangle 提交到 Vulkan 顶点缓冲（GPU 渲染），抗锯齿由 Nuklear 提供。
    class BlochSphereWindow : public IWindow
    {
    private:
        struct P { float x, y, depth; };
        struct V3 { float x, y, z; };

        int qubit_ = 0;
        float rot_yaw_ = 0.6f;    // 绕 Y 轴（经度）旋转角
        float rot_pitch_ = 0.45f; // 绕 X 轴（纬度）旋转角
        bool dragging_ = false;
        float last_mx_ = 0.0f, last_my_ = 0.0f;

        // 3D → 2D：先绕 X 轴（pitch），再绕 Y 轴（yaw），正交投影；depth 保留用于排序/光照。
        P project(float x, float y, float z, float cx, float cy) const
        {
            float y1 = y * cosf(rot_pitch_) - z * sinf(rot_pitch_);
            float z1 = y * sinf(rot_pitch_) + z * cosf(rot_pitch_);
            float x2 = x * cosf(rot_yaw_) + z1 * sinf(rot_yaw_);
            float z2 = -x * sinf(rot_yaw_) + z1 * cosf(rot_yaw_);
            float y2 = y1;
            return {cx + x2, cy - y2, z2};
        }

        void stroke(struct nk_command_buffer *canvas, const P &a, const P &b,
                    float w, struct nk_color c)
        {
            nk_stroke_line(canvas, a.x, a.y, b.x, b.y, w, c);
        }

    public:
        const char *title() const override { return tr("Bloch Sphere"); }

        void render(nk_context *ctx, const StateSnapshot &snap, float dt) override
        {
            (void)dt;
            if (nk_begin(ctx, title(), nk_rect(30, 370, 460, 340),
                         NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_CLOSABLE))
            {
                int nq = static_cast<int>(snap.num_qubits);
                if (nq <= 0 || snap.amplitudes.empty())
                {
                    nk_layout_row_dynamic(ctx, 20, 1);
                    nk_label(ctx, tr("Waiting for state data..."), NK_TEXT_LEFT);
                    nk_end(ctx);
                    return;
                }
                if (qubit_ >= nq)
                    qubit_ = 0;

                // 量子比特选择
                nk_layout_row_dynamic(ctx, 24, 3);
                char sel[32];
                snprintf(sel, sizeof(sel), tr("Qubit: %d"), qubit_);
                nk_label(ctx, sel, NK_TEXT_LEFT);
                if (nk_button_label(ctx, "<"))
                {
                    if (qubit_ > 0)
                        qubit_--;
                }
                if (nk_button_label(ctx, ">"))
                {
                    if (qubit_ + 1 < nq)
                        qubit_++;
                }

                // 约化密度矩阵 → 布洛赫向量
                size_t mask = 1ULL << qubit_;
                double rho00 = 0.0, rho11 = 0.0;
                std::complex<double> rho01{0.0, 0.0};
                size_t n = snap.amplitudes.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const auto &a = snap.amplitudes[i];
                    if (i & mask)
                    {
                        rho11 += std::norm(a);
                    }
                    else
                    {
                        rho00 += std::norm(a);
                        size_t j = i | mask;
                        if (j < n)
                            rho01 += a * std::conj(snap.amplitudes[j]);
                    }
                }
                float bx = 2.0f * static_cast<float>(rho01.real());
                float by = 2.0f * static_cast<float>(rho01.imag());
                float bz = static_cast<float>(rho00 - rho11);

                // NaN 防御：态向量异常时避免向 GPU 提交非法坐标导致崩溃
                if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz))
                {
                    bx = 0.0f;
                    by = 0.0f;
                    bz = 1.0f;
                }

                // 3D 视口
                nk_layout_space_begin(ctx, NK_STATIC, 235, INT_MAX);
                struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                struct nk_rect region = nk_layout_space_bounds(ctx);

                float cx = region.x + region.w / 2.0f;
                float cy = region.y + region.h / 2.0f;
                float r = (region.w < region.h ? region.w : region.h) / 2.0f - 8.0f;

                // 鼠标拖拽旋转（悬停在视口内才响应）
                struct nk_vec2 mouse = ctx->input.mouse.pos;
                if (nk_input_is_mouse_hovering_rect(&ctx->input, region) &&
                    nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
                {
                    if (!dragging_)
                    {
                        dragging_ = true;
                        last_mx_ = mouse.x;
                        last_my_ = mouse.y;
                    }
                    else
                    {
                        rot_yaw_ += (mouse.x - last_mx_) * 0.01f;
                        rot_pitch_ += (mouse.y - last_my_) * 0.01f;
                        last_mx_ = mouse.x;
                        last_my_ = mouse.y;
                    }
                }
                else
                {
                    dragging_ = false;
                }

                // ---- 实体球面（Lambert 光照 + 画家算法）----
                const int LAT = 12, LON = 18;
                const float PI = 3.14159265358979323846f;

                // UV 球体顶点
                std::vector<V3> verts;
                verts.reserve((LAT + 1) * LON);
                for (int i = 0; i <= LAT; ++i)
                {
                    float theta = PI * i / LAT;
                    for (int j = 0; j < LON; ++j)
                    {
                        float phi = 2.0f * PI * j / LON;
                        verts.push_back({r * sinf(theta) * cosf(phi),
                                         r * cosf(theta),
                                         r * sinf(theta) * sinf(phi)});
                    }
                }

                // 四边形 → 两个三角形；光照方向（归一化）
                V3 light = {-0.45f, 0.75f, 0.55f};
                float ll = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
                light.x /= ll; light.y /= ll; light.z /= ll;

                struct DrawTri { P a, b, c; float shade; float depth; };
                std::vector<DrawTri> draw;
                draw.reserve(LAT * LON * 2);

                for (int i = 0; i < LAT; ++i)
                {
                    for (int j = 0; j < LON; ++j)
                    {
                        int i00 = i * LON + j;
                        int i10 = (i + 1) * LON + j;
                        int i11 = (i + 1) * LON + ((j + 1) % LON);
                        int i01 = i * LON + ((j + 1) % LON);

                        const V3 tri[2][3] = {
                            {verts[i00], verts[i10], verts[i11]},
                            {verts[i00], verts[i11], verts[i01]}};

                        for (int k = 0; k < 2; ++k)
                        {
                            const V3 &a = tri[k][0];
                            const V3 &b = tri[k][1];
                            const V3 &c = tri[k][2];

                            // 面法线（叉积 + 归一化）
                            V3 u = {b.x - a.x, b.y - a.y, b.z - a.z};
                            V3 v = {c.x - a.x, c.y - a.y, c.z - a.z};
                            V3 normal = {u.y * v.z - u.z * v.y,
                                         u.z * v.x - u.x * v.z,
                                         u.x * v.y - u.y * v.x};
                            float nl = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                            if (nl < 1e-6f)
                                continue;
                            normal.x /= nl; normal.y /= nl; normal.z /= nl;

                            // Lambert 光照：ambient + diffuse
                            float diffuse = normal.x * light.x + normal.y * light.y + normal.z * light.z;
                            float shade = 0.30f + 0.70f * fmaxf(0.0f, diffuse);

                            P pa = project(a.x, a.y, a.z, cx, cy);
                            P pb = project(b.x, b.y, b.z, cx, cy);
                            P pc = project(c.x, c.y, c.z, cx, cy);
                            float depth = (pa.depth + pb.depth + pc.depth) / 3.0f;
                            draw.push_back({pa, pb, pc, shade, depth});
                        }
                    }
                }

                // 画家算法：depth 大（近相机）后画
                std::sort(draw.begin(), draw.end(), [](const DrawTri &x, const DrawTri &y)
                          { return x.depth < y.depth; });

                for (const auto &d : draw)
                {
                    int rr = static_cast<int>(38.0f * d.shade);
                    int gg = static_cast<int>(110.0f * d.shade);
                    int bb = static_cast<int>(175.0f * d.shade);
                    nk_fill_triangle(canvas, d.a.x, d.a.y, d.b.x, d.b.y, d.c.x, d.c.y,
                                     nk_rgb(rr, gg, bb));
                }

                // ---- 线框叠加（经纬线，增强结构感）----
                const struct nk_color grid = nk_rgb(150, 175, 205);
                for (int i = 1; i < 6; ++i)
                {
                    float theta = PI * i / 6;
                    float rr = r * sinf(theta);
                    float yy = r * cosf(theta);
                    for (int j = 0; j < LON; ++j)
                    {
                        float p0 = 2.0f * PI * j / LON;
                        float p1 = 2.0f * PI * (j + 1) / LON;
                        P a = project(rr * cosf(p0), yy, rr * sinf(p0), cx, cy);
                        P b = project(rr * cosf(p1), yy, rr * sinf(p1), cx, cy);
                        stroke(canvas, a, b, 1.0f, grid);
                    }
                }
                for (int j = 0; j < 6; ++j)
                {
                    float phi = PI * j / 6;
                    for (int i = 0; i < 24; ++i)
                    {
                        float t0 = PI * i / 24;
                        float t1 = PI * (i + 1) / 24;
                        P a = project(r * sinf(t0) * cosf(phi), r * cosf(t0), r * sinf(t0) * sinf(phi), cx, cy);
                        P b = project(r * sinf(t1) * cosf(phi), r * cosf(t1), r * sinf(t1) * sinf(phi), cx, cy);
                        stroke(canvas, a, b, 1.0f, grid);
                    }
                }

                // ---- 坐标轴：X 红 / Y 绿 / Z 蓝 ----
                P o = project(0, 0, 0, cx, cy);
                stroke(canvas, o, project(r, 0, 0, cx, cy), 1.8f, nk_rgb(235, 90, 90));
                stroke(canvas, o, project(0, r, 0, cx, cy), 1.8f, nk_rgb(90, 225, 90));
                stroke(canvas, o, project(0, 0, r, cx, cy), 1.8f, nk_rgb(100, 140, 250));

                // ---- 布洛赫向量（橙色 + 端点圆点）----
                P pv = project(bx * r, by * r, bz * r, cx, cy);
                stroke(canvas, o, pv, 2.4f, nk_rgb(245, 160, 70));
                nk_fill_circle(canvas, nk_rect(pv.x - 4.0f, pv.y - 4.0f, 8.0f, 8.0f), nk_rgb(245, 160, 70));

                nk_layout_space_end(ctx);

                // ---- 数值信息 ----
                nk_layout_row_dynamic(ctx, 18, 1);
                char vx[64], vy[64], vz[64];
                snprintf(vx, sizeof(vx), "<x> = %+.4f", bx);
                snprintf(vy, sizeof(vy), "<y> = %+.4f", by);
                snprintf(vz, sizeof(vz), "<z> = %+.4f", bz);
                nk_label(ctx, vx, NK_TEXT_LEFT);
                nk_label(ctx, vy, NK_TEXT_LEFT);
                nk_label(ctx, vz, NK_TEXT_LEFT);

                double theta = std::acos(std::max(-1.0, std::min(1.0, static_cast<double>(bz))));
                double phi = std::atan2(static_cast<double>(by), static_cast<double>(bx));
                char ang[96];
                snprintf(ang, sizeof(ang), "theta = %.3f rad,  phi = %.3f rad", theta, phi);
                nk_label(ctx, ang, NK_TEXT_LEFT);
                nk_label(ctx, tr("Drag to rotate"), NK_TEXT_CENTERED);
            }
            nk_end(ctx);
        }
    };
}