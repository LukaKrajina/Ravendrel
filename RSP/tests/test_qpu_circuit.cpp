// 量子处理器设计 + 机器人电路单元测试(去空壳)
#include "test_framework.hpp"
#include "qpu/qpu_design.hpp"
#include "circuit/robot_circuit.hpp"
#include "qhal/QVM.hpp"

using namespace rvdlrsp::qpu;
using namespace rvdlrsp::circuit;

// ─── QPU 拓扑邻接 ─────────────────────────────────────────

QTEST(qpu_adjacent_linear) {
    QPUSpec spec = QPUDesigner::design_qpu("lin", 5, QubitTopology::Linear);
    QCHECK(QPUDesigner::adjacent(spec, 0, 1));
    QCHECK(QPUDesigner::adjacent(spec, 1, 0)); // 对称
    QCHECK(!QPUDesigner::adjacent(spec, 0, 2));
    QCHECK(!QPUDesigner::adjacent(spec, 0, 4));
}

QTEST(qpu_adjacent_grid) {
    QPUSpec spec = QPUDesigner::design_qpu("grid", 4, QubitTopology::Grid);
    // 2×2 网格:0-1 同行,0-2 同列,0-3 对角
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

// ─── 最近邻路由 ───────────────────────────────────────────

QTEST(qpu_route_cnot_adjacent) {
    QPUSpec spec = QPUDesigner::design_qpu("lin", 5, QubitTopology::Linear);
    QPLProgram p = QPUDesigner::route_cnot(spec, 0, 1);
    QCHECK(p.ops.size() == 1);           // 相邻无需 SWAP
    QCHECK(p.ops[0].name == "CNOT");
}

QTEST(qpu_route_cnot_linear_routes) {
    QPUSpec spec = QPUDesigner::design_qpu("lin", 5, QubitTopology::Linear);
    QPLProgram p = QPUDesigner::route_cnot(spec, 0, 4); // 非相邻
    QCHECK(!p.ops.empty());
    QCHECK(p.ops.size() > 1);            // 含 SWAP + CNOT
    QCHECK(p.ops.back().name == "CNOT"); // 最后一个门是 CNOT
    // 每个门都物理相邻(路由正确性的关键)
    for (const auto &op : p.ops)
        QCHECK(QPUDesigner::adjacent(spec, static_cast<size_t>(op.target),
                                     static_cast<size_t>(op.control)));
}

QTEST(qpu_route_cnot_grid_routes) {
    QPUSpec spec = QPUDesigner::design_qpu("grid", 4, QubitTopology::Grid);
    QPLProgram p = QPUDesigner::route_cnot(spec, 0, 3); // 对角,非相邻
    QCHECK(!p.ops.empty());
    for (const auto &op : p.ops)
        QCHECK(QPUDesigner::adjacent(spec, static_cast<size_t>(op.target),
                                     static_cast<size_t>(op.control)));
}

// ─── 退相干误差 ───────────────────────────────────────────

QTEST(qpu_coherence_error) {
    QPUSpec spec{"t", 4, QubitTopology::Linear, 100.0, 0.1}; // T2=100us, 门=0.1us
    QPLProgram p;
    p.ops.push_back({"H", 0, -1, 0.0});       // 单 qubit
    p.ops.push_back({"CNOT", 1, 0, 0.0});     // 双 qubit
    double err = QPUDesigner::coherence_error(spec, p);
    // 0.1/100*1 + 0.1/100*2 = 0.003
    QCHECK_NEAR(err, 0.003, 1e-9);
}

// ─── 程序生成与执行 ───────────────────────────────────────

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
    // Bell 态 (|00>+|11>)/√2:每个 qubit 的 ⟨Z⟩ = 0(等概率 0/1,非破坏)
    double ez = qvm.expectation_z(0);
    QCHECK_NEAR(ez, 0.0, 0.05);
}

// ─── 机器人电路 ───────────────────────────────────────────

QTEST(circuit_add_component) {
    RobotCircuit c;
    int a = c.add_component("sensor", 0, 2);     // 2 输出
    int b = c.add_component("processor", 1, 1);  // 1 输入 1 输出
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

    // 连通性
    QCHECK(c.connected(a, d));
    QCHECK(c.connected_directly(a, b));

    // 拓扑排序:sensor → processor → actuator
    auto order = c.topological_order();
    QCHECK(order.size() == 3);
    QCHECK(order[0] == a);
    QCHECK(order[2] == d);
}

QTEST(circuit_invalid_connect_rejected) {
    RobotCircuit c;
    int a = c.add_component("sensor", 0, 1);
    c.add_component("actuator", 1, 0);
    // 越界引脚应被拒绝
    QCHECK(!c.connect(a, 5, 1, 0));
    QCHECK(!c.connect(a, 0, 1, 3));
    QCHECK(c.wire_count() == 0);
}

QTEST(circuit_dangling_pins) {
    RobotCircuit c;
    c.add_component("sensor", 0, 1);      // 输出未连接
    int b = c.add_component("processor", 1, 1);
    c.add_component("actuator", 1, 0);    // 输入未连接
    // 只有 processor 的输入连接了 sensor 的输出
    c.connect(0, 0, b, 0);
    auto dangling = c.dangling_pins();
    // sensor 输出已连;processor 输出未连;actuator 输入未连 → 2 个悬空
    QCHECK(dangling.size() == 2);
}