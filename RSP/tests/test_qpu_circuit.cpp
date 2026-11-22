#include "test_framework.hpp"
#include "qpu/qpu_design.hpp"
#include "circuit/robot_circuit.hpp"
#include "qhal/QVM.hpp"

using namespace rvdlrsp::qpu;
using namespace rvdlrsp::circuit;


QTEST(qpu_adjacent_linear) {
    QPUSpec spec = QPUDesigner::design_qpu("lin", 5, QubitTopology::Linear);
    QCHECK(QPUDesigner::adjacent(spec, 0, 1));
    QCHECK(QPUDesigner::adjacent(spec, 1, 0));
    QCHECK(!QPUDesigner::adjacent(spec, 0, 2));
    QCHECK(!QPUDesigner::adjacent(spec, 0, 4));
}

QTEST(qpu_adjacent_grid) {
    QPUSpec spec = QPUDesigner::design_qpu("grid", 4, QubitTopology::Grid);
    QCHECK(QPUDesigner::adjacent(spec, 0, 1));
    QCHECK(QPUDesigner::adjacent(spec, 0, 2));
    QCHECK(QPUDesigner::adjacent(spec, 1, 3));
    QCHECK(!QPUDesigner::adjacent(spec, 0, 3));
}

QTEST(qpu_adjacent_alltoall) {
    QPUSpec spec = QPUDesigner::design_qpu("a2a", 8, QubitTopology::AllToAll);
    QCHECK(QPUDesigner::adjacent(spec, 0, 7));
    QCHECK(QPUDesigner::adjacent(spec, 3, 5));
}


QTEST(qpu_route_cnot_adjacent) {
    QPUSpec spec = QPUDesigner::design_qpu("lin", 5, QubitTopology::Linear);
    QPLProgram p = QPUDesigner::route_cnot(spec, 0, 1);
    QCHECK(p.ops.size() == 1);
    QCHECK(p.ops[0].name == "CNOT");
}

QTEST(qpu_route_cnot_linear_routes) {
    QPUSpec spec = QPUDesigner::design_qpu("lin", 5, QubitTopology::Linear);
    QPLProgram p = QPUDesigner::route_cnot(spec, 0, 4);
    QCHECK(!p.ops.empty());
    QCHECK(p.ops.size() > 1);
    QCHECK(p.ops.back().name == "CNOT");
    for (const auto &op : p.ops)
        QCHECK(QPUDesigner::adjacent(spec, static_cast<size_t>(op.target),
                                     static_cast<size_t>(op.control)));
}

QTEST(qpu_route_cnot_grid_routes) {
    QPUSpec spec = QPUDesigner::design_qpu("grid", 4, QubitTopology::Grid);
    QPLProgram p = QPUDesigner::route_cnot(spec, 0, 3);
    QCHECK(!p.ops.empty());
    for (const auto &op : p.ops)
        QCHECK(QPUDesigner::adjacent(spec, static_cast<size_t>(op.target),
                                     static_cast<size_t>(op.control)));
}


QTEST(qpu_coherence_error) {
    QPUSpec spec{"t", 4, QubitTopology::Linear, 100.0, 0.1};
    QPLProgram p;
    p.ops.push_back({"H", 0, -1, 0.0});
    p.ops.push_back({"CNOT", 1, 0, 0.0});
    double err = QPUDesigner::coherence_error(spec, p);
    QCHECK_NEAR(err, 0.003, 1e-9);
}


QTEST(qpu_design_qpl_bell) {
    QPLProgram p = QPUDesigner::design_qpl("bell");
    QCHECK(p.ops.size() == 2);
    QCHECK(p.ops[0].name == "H");
    QCHECK(p.ops[1].name == "CNOT");
}

QTEST(qpu_execute_bell) {
    qhal::QVM qvm;
    qvm.allocate_qubits(2);
    QPLProgram p = QPUDesigner::design_qpl("bell");
    QPUDesigner::execute(p, qvm);
    double ez = qvm.expectation_z(0);
    QCHECK_NEAR(ez, 0.0, 0.05);
}


QTEST(circuit_add_component) {
    RobotCircuit c;
    int a = c.add_component("sensor", 0, 2);
    int b = c.add_component("processor", 1, 1);
    QCHECK(a == 0);
    QCHECK(b == 1);
    QCHECK(c.node_count() == 2);
    QCHECK(c.nodes()[0].output_pins.size() == 2);
    QCHECK(c.nodes()[1].input_pins.size() == 1);
}

QTEST(circuit_connect_and_topo) {
    RobotCircuit c;
    int a = c.add_component("sensor", 0, 1);
    int b = c.add_component("processor", 1, 1);
    int d = c.add_component("actuator", 1, 0);
    QCHECK(c.connect(a, 0, b, 0));
    QCHECK(c.connect(b, 0, d, 0));
    QCHECK(c.wire_count() == 2);

    QCHECK(c.connected(a, d));
    QCHECK(c.connected_directly(a, b));

    auto order = c.topological_order();
    QCHECK(order.size() == 3);
    QCHECK(order[0] == a);
    QCHECK(order[2] == d);
}

QTEST(circuit_invalid_connect_rejected) {
    RobotCircuit c;
    int a = c.add_component("sensor", 0, 1);
    c.add_component("actuator", 1, 0);
    QCHECK(!c.connect(a, 5, 1, 0));
    QCHECK(!c.connect(a, 0, 1, 3));
    QCHECK(c.wire_count() == 0);
}

QTEST(circuit_dangling_pins) {
    RobotCircuit c;
    c.add_component("sensor", 0, 1);
    int b = c.add_component("processor", 1, 1);
    c.add_component("actuator", 1, 0);
    c.connect(0, 0, b, 0);
    auto dangling = c.dangling_pins();
    QCHECK(dangling.size() == 2);
}