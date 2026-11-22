#include "test_framework.hpp"
#include "qhal/GateCommutation.hpp"

using namespace qhal;

QTEST(pauli_anticommute) {
    QCHECK_NEAR(pauli_commutation_phase(GateType::X, GateType::Z), 3.14159265358979323846, 1e-9);
    QCHECK_NEAR(pauli_commutation_phase(GateType::X, GateType::Y), 3.14159265358979323846, 1e-9);
    QCHECK_NEAR(pauli_commutation_phase(GateType::Y, GateType::Z), 3.14159265358979323846, 1e-9);
}

QTEST(pauli_self_commute) {
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
    ScheduledGate a{GateType::X, 0, 0, 0.0};
    ScheduledGate b{GateType::Z, 1, 0, 0.0};
    QCHECK(gates_commute_scoped(a, b));
    ScheduledGate c{GateType::Z, 0, 0, 0.0};
    QCHECK(!gates_commute_scoped(a, c));
    ScheduledGate d{GateType::X, 0, 0, 0.0};
    QCHECK(gates_commute_scoped(a, d));
}