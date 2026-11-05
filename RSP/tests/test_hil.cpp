// 硬件在环（HIL）测试脚手架单元测试
#include "test_framework.hpp"
#include "hardware/hil_harness.hpp"

using namespace rvdlrsp::hardware;

QTEST(signal_recorder_record) {
    SignalRecorder rec;
    rec.start();
    rec.record(0.0, {0.5, 0.3}, {1.0, 2.0}, {0.9, 1.9});
    rec.record(0.1, {0.6, 0.4}, {1.1, 2.1}, {1.0, 2.0});
    QCHECK(rec.size() == 2);
    QCHECK(rec.samples()[1].t == 0.1);
}

QTEST(latency_meter_measure) {
    LatencyMeter lm;
    lm.measure(0.0, 0.05);
    lm.measure(0.1, 0.13);
    QCHECK_NEAR(lm.average(), 0.04, 1e-9);
    QCHECK_NEAR(lm.max(), 0.05, 1e-9);
    QCHECK_NEAR(lm.min(), 0.03, 1e-9);
}

QTEST(closed_loop_validator) {
    ClosedLoopValidator v(0.2);
    v.validate({1.0, 2.0}, {1.1, 2.1}); // 偏差 0.1 一致
    QCHECK(v.failure_count() == 0);
    v.validate({1.0}, {1.5});           // 偏差 0.5 不一致
    QCHECK(v.failure_count() == 1);
}

QTEST(hil_harness_report_pass) {
    HilHarness h(0.2);
    // 模拟 10 次正常闭环（延迟 0.02，反馈一致）
    for (int i = 0; i < 10; ++i)
        h.step(i * 0.1, i * 0.1, i * 0.1 + 0.02,
               {0.5}, {1.0}, {0.98});
    auto r = h.report(0.5);
    QCHECK(r.sample_count == 10);
    QCHECK(r.closed_loop_failures == 0);
    QCHECK(r.passed);
}

QTEST(hil_harness_report_fail_latency) {
    HilHarness h(0.2);
    // 延迟超限
    h.step(0.0, 0.0, 0.8, {0.5}, {1.0}, {1.0});
    auto r = h.report(0.5);
    QCHECK(r.max_latency > 0.5);
    QCHECK(!r.passed);
}