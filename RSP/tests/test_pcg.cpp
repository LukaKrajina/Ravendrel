// PCG 框架单元测试
#include "test_framework.hpp"
#include "pcg/pcg_framework.hpp"

using namespace rvdlrsp::pcg;

QTEST(pcg_seed_deterministic) {
    PCGFramework a(42);
    PCGFramework b(42);
    // 同种子应产生相同序列
    for (int i = 0; i < 10; ++i)
        QCHECK_NEAR(a.next_unit(), b.next_unit(), 1e-12);
}

QTEST(pcg_int_range) {
    PCGFramework pcg(7);
    for (int i = 0; i < 100; ++i) {
        int v = pcg.next_int(-5, 5);
        QCHECK(v >= -5 && v <= 5);
    }
}
