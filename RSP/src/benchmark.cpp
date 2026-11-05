// rvdlRSP 性能基准
// 覆盖:物理引擎 step、碰撞检测、量子门路由。
// 输出各子项的耗时,供性能回归对比。
#include "qpc/physics_kernel.hpp"
#include "qpc/collision.hpp"
#include "qpu/qpu_design.hpp"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>

using namespace rvdlrsp;

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ─── 物理引擎 step ────────────────────────────────────
static void bench_physics()
{
    qpc::PhysicsKernel kernel(false, 1.0 / 60.0);
    kernel.set_gravity({0, -9.81, 0});
    const int N = 500;
    for (int i = 0; i < N; ++i)
    {
        qpc::RigidBody b;
        b.set_mass(1.0);
        b.position = {(i % 20) * 1.0 - 10.0, (i / 20) * 1.0 + 1.0, 0.0};
        qpc::Collider c;
        c.type = qpc::ShapeType::Sphere;
        c.radius = 0.3;
        kernel.add_body(b, c);
    }

    const int steps = 100;
    auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i)
        kernel.step();
    double ms = ms_since(t0);
    std::cout << "  physics step x" << steps << " (" << N << " spheres): "
              << std::fixed << std::setprecision(3) << ms << " ms ("
              << ms / steps << " ms/step)\n";
}

// ─── 碰撞检测(全对球-球)───────────────────────────────
static void bench_collision()
{
    const int N = 1000;
    std::vector<qpc::RigidBody> bodies(N);
    std::vector<qpc::Collider> colliders(N);
    for (int i = 0; i < N; ++i)
    {
        bodies[i].set_mass(1.0);
        bodies[i].position = {(i % 40) * 1.0, (i / 40) * 1.0, 0.0};
        colliders[i].type = qpc::ShapeType::Sphere;
        colliders[i].radius = 0.3;
        colliders[i].body_index = i;
    }

    int hits = 0;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
        {
            qpc::Contact c;
            if (qpc::CollisionDetector::sphere_sphere(bodies[i], colliders[i],
                                                      bodies[j], colliders[j], c))
                ++hits;
        }
    double ms = ms_since(t0);
    std::cout << "  sphere-sphere x" << (N * (N - 1) / 2) << " pairs: "
              << std::fixed << std::setprecision(3) << ms << " ms ("
              << hits << " hits)\n";
}

// ─── 量子门路由(Linear 拓扑)───────────────────────────
static void bench_quantum_route()
{
    qpu::QPUSpec spec = qpu::QPUDesigner::design_qpu("bench", 64, qpu::QubitTopology::Linear);
    const int routes = 1000;
    auto t0 = Clock::now();
    for (int i = 0; i < routes; ++i)
    {
        int c = i % 32;
        int t = (i % 32) + 32;
        qpu::QPUDesigner::route_cnot(spec, c, t);
    }
    double ms = ms_since(t0);
    std::cout << "  quantum route x" << routes << " (64-qubit Linear): "
              << std::fixed << std::setprecision(3) << ms << " ms ("
              << ms / routes << " ms/route)\n";
}

int main()
{
    std::cout << "=== rvdlRSP benchmark ===\n";
    bench_physics();
    bench_collision();
    bench_quantum_route();
    std::cout << "=== done ===\n";
    return 0;
}
