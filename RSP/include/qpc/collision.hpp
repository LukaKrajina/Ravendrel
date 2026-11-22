#pragma once
#include <cstdint>
#include <algorithm>
#include "math.hpp"
#include "rigid_body.hpp"
#include "convex_hull.hpp"

namespace rvdlrsp::qpc
{

    enum class ShapeType
    {
        Sphere,
        AABB,
        Capsule,
        Cylinder,
        ConvexHull
    };

    struct Collider
    {
        ShapeType type = ShapeType::Sphere;
        double radius = 0.5;
        Vec3 half_extents{0.5, 0.5, 0.5};
        double capsule_half_height = 0.5;
        ConvexHull hull;
        size_t body_index = 0;
    };

    struct Contact
    {
        size_t a = 0, b = 0;
        Vec3 normal;
        double penetration = 0.0;
        Vec3 point;
    };

    class CollisionDetector
    {
    public:
        static bool sphere_sphere(const RigidBody &a, const Collider &ca,
                                  const RigidBody &b, const Collider &cb,
                                  Contact &out)
        {
            Vec3 delta = b.position - a.position;
            double dist2 = delta.length_sq();
            double r = ca.radius + cb.radius;
            if (dist2 >= r * r || dist2 < 1e-12)
                return false;
            double dist = std::sqrt(dist2);
            out.a = ca.body_index;
            out.b = cb.body_index;
            out.normal = delta / dist;
            out.penetration = r - dist;
            out.point = a.position + out.normal * ca.radius;
            return true;
        }

        static bool sphere_aabb(const RigidBody &sphere, const Collider &cs,
                                const RigidBody &box, const Collider &cb,
                                Contact &out)
        {
            Vec3 closest;
            closest.x = std::max(-cb.half_extents.x, std::min(sphere.position.x, cb.half_extents.x));
            closest.y = std::max(-cb.half_extents.y, std::min(sphere.position.y, cb.half_extents.y));
            closest.z = std::max(-cb.half_extents.z, std::min(sphere.position.z, cb.half_extents.z));

            Vec3 delta = sphere.position - closest;
            double dist2 = delta.length_sq();
            if (dist2 >= cs.radius * cs.radius)
                return false;
            double dist = std::sqrt(dist2);
            out.a = cs.body_index;
            out.b = cb.body_index;
            out.normal = (dist > 1e-12) ? (closest - sphere.position) / dist : Vec3{0, -1, 0};
            out.penetration = cs.radius - dist;
            out.point = closest;
            return true;
        }

        static bool aabb_aabb(const RigidBody &a, const Collider &ca,
                              const RigidBody &b, const Collider &cb,
                              Contact &out)
        {
            Vec3 delta = b.position - a.position;
            Vec3 overlap = (ca.half_extents + cb.half_extents) -
                           Vec3{std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)};
            if (overlap.x <= 0 || overlap.y <= 0 || overlap.z <= 0)
                return false;

            if (overlap.x < overlap.y && overlap.x < overlap.z)
            {
                out.normal = Vec3{(delta.x >= 0) ? 1.0 : -1.0, 0, 0};
                out.penetration = overlap.x;
            }
            else if (overlap.y < overlap.z)
            {
                out.normal = Vec3{0, (delta.y >= 0) ? 1.0 : -1.0, 0};
                out.penetration = overlap.y;
            }
            else
            {
                out.normal = Vec3{0, 0, (delta.z >= 0) ? 1.0 : -1.0};
                out.penetration = overlap.z;
            }
            out.a = ca.body_index;
            out.b = cb.body_index;
            out.point = (a.position + b.position) * 0.5;
            return true;
        }

        static void capsule_segment(const RigidBody &body, const Collider &c,
                                    Vec3 &p0, Vec3 &p1)
        {
            Vec3 half{0, c.capsule_half_height, 0};
            p0 = body.position - half;
            p1 = body.position + half;
        }

        static Vec3 closest_point_on_segment(const Vec3 &p, const Vec3 &a, const Vec3 &b)
        {
            Vec3 ab = b - a;
            double t = ab.dot(p - a) / ab.dot(ab);
            t = std::max(0.0, std::min(1.0, t));
            return a + ab * t;
        }

        static double segment_segment_distance(const Vec3 &p1, const Vec3 &p2,
                                               const Vec3 &q1, const Vec3 &q2,
                                               Vec3 &c1, Vec3 &c2)
        {
            Vec3 d1 = p2 - p1;
            Vec3 d2 = q2 - q1;
            Vec3 r = p1 - q1;
            double a = d1.dot(d1);
            double e = d2.dot(d2);
            double f = d2.dot(r);
            double s, t;

            if (a <= 1e-12 && e <= 1e-12)
            {
                c1 = p1;
                c2 = q1;
                return (c1 - c2).length();
            }
            if (a <= 1e-12)
            {
                s = 0.0;
                t = std::max(0.0, std::min(1.0, f / e));
            }
            else
            {
                double c = d1.dot(r);
                if (e <= 1e-12)
                {
                    t = 0.0;
                    s = std::max(0.0, std::min(1.0, -c / a));
                }
                else
                {
                    double b = d1.dot(d2);
                    double denom = a * e - b * b;
                    s = (denom > 1e-12) ? std::max(0.0, std::min(1.0, (b * f - c * e) / denom)) : 0.0;
                    t = (b * s + f) / e;
                    if (t < 0.0)
                    {
                        t = 0.0;
                        s = std::max(0.0, std::min(1.0, -c / a));
                    }
                    else if (t > 1.0)
                    {
                        t = 1.0;
                        s = std::max(0.0, std::min(1.0, (b - c) / a));
                    }
                }
            }
            c1 = p1 + d1 * s;
            c2 = q1 + d2 * t;
            return (c1 - c2).length();
        }

        static bool capsule_capsule(const RigidBody &a, const Collider &ca,
                                    const RigidBody &b, const Collider &cb,
                                    Contact &out)
        {
            Vec3 a0, a1, b0, b1;
            capsule_segment(a, ca, a0, a1);
            capsule_segment(b, cb, b0, b1);
            Vec3 c1, c2;
            double dist = segment_segment_distance(a0, a1, b0, b1, c1, c2);
            double r = ca.radius + cb.radius;
            if (dist >= r || dist < 1e-12)
                return false;

            out.a = ca.body_index;
            out.b = cb.body_index;
            out.normal = (c2 - c1).normalized();
            out.penetration = r - dist;
            out.point = (c1 + c2) * 0.5;
            return true;
        }

        static bool capsule_aabb(const RigidBody &cap, const Collider &cc,
                                 const RigidBody &box, const Collider &cb,
                                 Contact &out)
        {
            Vec3 p0, p1;
            capsule_segment(cap, cc, p0, p1);
            Vec3 closest_on_seg = closest_point_on_segment(box.position, p0, p1);
            Vec3 closest_on_box;
            closest_on_box.x = std::max(box.position.x - cb.half_extents.x,
                                        std::min(closest_on_seg.x, box.position.x + cb.half_extents.x));
            closest_on_box.y = std::max(box.position.y - cb.half_extents.y,
                                        std::min(closest_on_seg.y, box.position.y + cb.half_extents.y));
            closest_on_box.z = std::max(box.position.z - cb.half_extents.z,
                                        std::min(closest_on_seg.z, box.position.z + cb.half_extents.z));

            Vec3 delta = closest_on_seg - closest_on_box;
            double dist = delta.length();
            if (dist >= cc.radius)
                return false;

            out.a = cc.body_index;
            out.b = cb.body_index;
            out.normal = (dist > 1e-12) ? (closest_on_box - closest_on_seg) / dist : Vec3{0, -1, 0};
            out.penetration = cc.radius - dist;
            out.point = closest_on_box;
            return true;
        }

        static bool capsule_sphere(const RigidBody &cap, const Collider &cc,
                                   const RigidBody &sph, const Collider &cs,
                                   Contact &out)
        {
            Vec3 p0, p1;
            capsule_segment(cap, cc, p0, p1);
            Vec3 closest = closest_point_on_segment(sph.position, p0, p1);
            Vec3 delta = sph.position - closest;
            double dist = delta.length();
            double r = cc.radius + cs.radius;
            if (dist >= r || dist < 1e-12)
                return false;

            out.a = cc.body_index;
            out.b = cs.body_index;
            out.normal = delta / dist;
            out.penetration = r - dist;
            out.point = closest;
            return true;
        }

        static Vec3 closest_point_on_cylinder(const Vec3 &p, double radius, double half_height)
        {
            double y = std::max(-half_height, std::min(p.y, half_height));
            double dx = p.x, dz = p.z;
            double d2 = dx * dx + dz * dz;
            if (d2 > radius * radius)
            {
                double d = std::sqrt(d2);
                double s = radius / d;
                return {dx * s, y, dz * s};
            }
            return {p.x, y, p.z};
        }

        static bool cylinder_sphere(const RigidBody &cyl, const Collider &cc,
                                    const RigidBody &sph, const Collider &cs,
                                    Contact &out)
        {
            Vec3 closest = closest_point_on_cylinder(sph.position, cc.radius, cc.capsule_half_height);
            Vec3 delta = sph.position - closest;
            double dist = delta.length();
            if (dist >= cs.radius || dist < 1e-12)
                return false;

            out.a = cc.body_index;
            out.b = cs.body_index;
            out.normal = delta / dist;
            out.penetration = cs.radius - dist;
            out.point = closest;
            return true;
        }

        static bool cylinder_cylinder(const RigidBody &a, const Collider &ca,
                                      const RigidBody &b, const Collider &cb,
                                      Contact &out)
        {
            Vec3 a0, a1, b0, b1;
            capsule_segment(a, ca, a0, a1);
            capsule_segment(b, cb, b0, b1);
            Vec3 c1, c2;
            double dist = segment_segment_distance(a0, a1, b0, b1, c1, c2);
            double r = ca.radius + cb.radius;
            if (dist >= r || dist < 1e-12)
                return false;

            out.a = ca.body_index;
            out.b = cb.body_index;
            out.normal = (c2 - c1).normalized();
            out.penetration = r - dist;
            out.point = (c1 + c2) * 0.5;
            return true;
        }

        static bool cylinder_capsule(const RigidBody &cyl, const Collider &cc,
                                     const RigidBody &cap, const Collider &cp,
                                     Contact &out)
        {
            return capsule_capsule(cyl, cc, cap, cp, out);
        }

        static bool cylinder_aabb(const RigidBody &cyl, const Collider &cc,
                                  const RigidBody &box, const Collider &cb,
                                  Contact &out)
        {
            return capsule_aabb(cyl, cc, box, cb, out);
        }

        static bool convex_convex(const RigidBody &a, const Collider &ca,
                                  const RigidBody &b, const Collider &cb, Contact &out)
        {
            ConvexHull wa = transform_hull(ca.hull, a.position, a.orientation);
            ConvexHull wb = transform_hull(cb.hull, b.position, b.orientation);

            Vec3 cA = wa.center(), cB = wb.center();
            double min_overlap = 1e30;
            Vec3 best_axis{0, 1, 0};
            bool has = false;

            auto test = [&](const Vec3 &axis_raw) -> bool
            {
                Vec3 ax = axis_raw.normalized();
                if (ax.length_sq() < 1e-12)
                    return true;
                double a_min = 1e30, a_max = -1e30;
                for (const auto &v : wa.vertices)
                {
                    double d = v.dot(ax);
                    a_min = std::min(a_min, d);
                    a_max = std::max(a_max, d);
                }
                double b_min = 1e30, b_max = -1e30;
                for (const auto &v : wb.vertices)
                {
                    double d = v.dot(ax);
                    b_min = std::min(b_min, d);
                    b_max = std::max(b_max, d);
                }
                double overlap = std::min(a_max, b_max) - std::max(a_min, b_min);
                if (overlap < 0)
                    return false;
                if (overlap < min_overlap)
                {
                    min_overlap = overlap;
                    best_axis = (cB - cA).dot(ax) >= 0 ? ax : -ax;
                    has = true;
                }
                return true;
            };

            for (const auto &n : wa.face_normals)
                if (!test(n))
                    return false;
            for (const auto &n : wb.face_normals)
                if (!test(n))
                    return false;
            if (!has)
                return false;

            out.a = ca.body_index;
            out.b = cb.body_index;
            out.normal = best_axis;
            out.penetration = min_overlap;
            out.point = (cA + cB) * 0.5;
            return true;
        }

        static bool convex_sphere(const RigidBody &hb, const Collider &ch,
                                  const RigidBody &sph, const Collider &cs, Contact &out)
        {
            ConvexHull wh = transform_hull(ch.hull, hb.position, hb.orientation);
            Vec3 closest;
            double dist = closest_point_on_convex(wh, sph.position, closest);
            if (dist >= cs.radius || dist < 1e-12)
                return false;

            Vec3 delta = sph.position - closest;
            out.a = ch.body_index;
            out.b = cs.body_index;
            out.normal = delta / dist;
            out.penetration = cs.radius - dist;
            out.point = closest;
            return true;
        }

        static bool convex_aabb(const RigidBody &hb, const Collider &ch,
                                const RigidBody &box, const Collider &cb, Contact &out)
        {
            ConvexHull boxhull;
            const Vec3 &h = cb.half_extents;
            for (int i = 0; i < 8; ++i)
            {
                Vec3 v;
                v.x = (i & 1) ? h.x : -h.x;
                v.y = (i & 2) ? h.y : -h.y;
                v.z = (i & 4) ? h.z : -h.z;
                boxhull.vertices.push_back(v);
            }
            static const uint32_t box_faces[36] = {
                0, 2, 1, 0, 3, 2,
                4, 5, 6, 4, 6, 7,
                0, 1, 5, 0, 5, 4,
                2, 7, 3, 2, 6, 7,
                0, 3, 7, 0, 7, 4,
                1, 2, 6, 1, 6, 5};
            for (uint32_t f : box_faces)
                boxhull.faces.push_back(f);
            static const Vec3 box_normals[6] = {
                {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
            for (int f = 0; f < 6; ++f)
                for (int t = 0; t < 2; ++t)
                    boxhull.face_normals.push_back(box_normals[f]);

            RigidBody tmp = box;
            Collider tc;
            tc.type = ShapeType::ConvexHull;
            tc.hull = boxhull;
            tc.body_index = cb.body_index;
            return convex_convex(hb, ch, tmp, tc, out);
        }

        static bool convex_capsule(const RigidBody &hb, const Collider &ch,
                                   const RigidBody &cap, const Collider &cp, Contact &out)
        {
            Collider sc;
            sc.type = ShapeType::Sphere;
            sc.radius = cp.radius + cp.capsule_half_height;
            sc.body_index = cp.body_index;
            return convex_sphere(hb, ch, cap, sc, out);
        }

        static bool convex_cylinder(const RigidBody &hb, const Collider &ch,
                                    const RigidBody &cyl, const Collider &cc, Contact &out)
        {
            Collider sc;
            sc.type = ShapeType::Sphere;
            sc.radius = cc.radius + cc.capsule_half_height;
            sc.body_index = cc.body_index;
            return convex_sphere(hb, ch, cyl, sc, out);
        }
    };
}