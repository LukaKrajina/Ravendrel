// 门交换代数单元测试
#include "test_framework.hpp"
#include "qhal/GateCommutation.hpp"

using namespace qhal;

QTEST(pauli_anticommute) {
    // X, Y, Z 两两反对易：g_a g_b = e^{iπ} g_b g_a，相位 π
    QCHECK_NEAR(pauli_commutation_phase(GateType::X, GateType::Z), 3.14159265358979323846, 1e-9);
    QCHECK_NEAR(pauli_commutation_phase(GateType::X, GateType::Y), 3.14159265358979323846, 1e-9);
    QCHECK_NEAR(pauli_commutation_phase(GateType::Y, GateType::Z), 3.14159265358979323846, 1e-9);
}

QTEST(pauli_self_commute) {
    // 相同 Pauli / 非 Pauli 门对易（相位 0）
    QCHECK_NEAR(pauli_commutation_phase(GateType::X, GateType::X), 0.0, 1e-9);
    QCHECK(gates_commute(GateType::X, GateType::X));
    QCHECK(gates_commute(GateType::H, GateType::H));
    QCHECK(gates_commute(GateType::H, GateType::T));
}

QTEST(pauli_noncommute) {
    QCHECK(!gates_commute(GateType::X, GateType::Z));
    QCHECK(!gates_commute(GateType::Y, GateType::X));
}

QTEST(scoped_disjoint_commute) {
    // 作用不同比特的门天然可交换
    ScheduledGate a{GateType::X, 0, 0, 0.0};
    ScheduledGate b{GateType::Z, 1, 0, 0.0};
    QCHECK(gates_commute_scoped(a, b));
    // 作用相同比特的 X 与 Z 不可交换
    ScheduledGate c{GateType::Z, 0, 0, 0.0};
    QCHECK(!gates_commute_scoped(a, c));
    // 作用相同比特的相同门可交换
    ScheduledGate d{GateType::X, 0, 0, 0.0};
    QCHECK(gates_commute_scoped(a, d));
}
