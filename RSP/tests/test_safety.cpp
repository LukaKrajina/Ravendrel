#include "test_framework.hpp"
#include "hardware/fault_detection.hpp"
#include "hardware/observability.hpp"
#include "qpc/broadphase.hpp"
#include "control/async_pipeline.hpp"

#include <thread>
#include <chrono>
#include <atomic>

using namespace rvdlrsp::hardware;
using namespace rvdlrsp::qpc;
using namespace rvdlrsp::control;

QTEST(heartbeat_alive) {
    HeartbeatMonitor hb(0.1);
    QCHECK(hb.alive());
}

QTEST(watchdog_timeout) {
    int fired = 0;
    Watchdog wd(0.05, [&] { ++fired; });
    QCHECK(!wd.check());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    QCHECK(wd.check());
    QCHECK(fired == 1);
    QCHECK(wd.check());
    QCHECK(fired == 1);
}

QTEST(encoder_consistency) {
    EncoderConsistencyCheck ec(0.2);
    QCHECK(ec.validate(1.0, 1.1));
    QCHECK(!ec.validate(1.0, 1.5));
}

QTEST(dual_channel_redundancy) {
    DualChannelRedundancy dc(0.1);
    QCHECK(dc.agree(0.5, 0.55));
    QCHECK(!dc.agree(0.5, 0.8));
}

QTEST(logger_levels) {
    Logger log;
    log.set_level(LogLevel::Warn);
    log.info("test", "should be filtered");
    log.error("test", "boom");
    QCHECK(log.error_count() == 1);
    QCHECK(log.warn_count() == 0);
}

QTEST(metrics_counter) {
    MetricsRegistry m;
    m.inc_counter("steps", 1);
    m.inc_counter("steps", 2);
    QCHECK(m.get_counter("steps") == 3);
    m.set_gauge("temp", 36.5);
    QCHECK_NEAR(m.get_gauge("temp"), 36.5, 1e-9);
}

QTEST(broadphase_query) {
    BroadPhase bp;
    std::vector<Aabb> boxes = {
        Aabb({0, 0, 0}, {1, 1, 1}),
        Aabb({0.5, 0.5, 0.5}, {1.5, 1.5, 1.5}),
        Aabb({5, 5, 5}, {6, 6, 6}),
    };
    bp.build(boxes);
    QCHECK(bp.node_count() > 0);
    auto pairs = bp.query_pairs();
    QCHECK(pairs.size() == 1);
}

QTEST(physics_worker_runs) {
    std::atomic<int> steps{0};
    PhysicsWorker worker(200.0, [&](double) { ++steps; });
    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    worker.stop();
    QCHECK(steps.load() > 0);
}

QTEST(double_buffer_snapshot) {
    DoubleBuffer<int> db;
    db.acquire_write() = 42;
    db.swap();
    QCHECK(db.snapshot() == 42);
}