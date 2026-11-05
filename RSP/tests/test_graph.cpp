// 节点图模型单元测试
#include "test_framework.hpp"
#include "blueprint/graph.hpp"

using namespace rvdlrsp::blueprint;

QTEST(graph_add_node) {
    NodeGraph g;
    NodeId id = g.add_node("Sequence", "behavior_tree",
                           {{"enter", "exec"}},
                           {{"success", "exec"}}, 10, 20);
    QCHECK(id != 0);
    QCHECK(g.node_count() == 1);
    const Node *n = g.find_node(id);
    QCHECK(n != nullptr);
    QCHECK(n->inputs.size() == 1);
    QCHECK(n->outputs.size() == 1);
    QCHECK(n->inputs[0].name == "enter");
    QCHECK(n->outputs[0].name == "success");
}

QTEST(graph_connect) {
    NodeGraph g;
    NodeId a = g.add_node("A", "test", {}, {{"out", "float"}});
    NodeId b = g.add_node("B", "test", {{"in", "float"}}, {});
    g.connect(a, "out", b, "in");
    QCHECK(g.edge_count() == 1);
}

QTEST(graph_connect_pins) {
    NodeGraph g;
    NodeId a = g.add_node("A", "test", {}, {{"out", "float"}});
    NodeId b = g.add_node("B", "test", {{"in", "float"}}, {});
    PinId out_pin = g.find_node(a)->outputs[0].id;
    PinId in_pin = g.find_node(b)->inputs[0].id;
    g.connect_pins(out_pin, in_pin);
    QCHECK(g.edge_count() == 1);
    QCHECK(g.edges()[0].from == out_pin);
    QCHECK(g.edges()[0].to == in_pin);
}

QTEST(graph_find_pin) {
    NodeGraph g;
    NodeId a = g.add_node("A", "test", {{"x", "float"}}, {});
    PinId pin = g.find_node(a)->inputs[0].id;
    const Pin *p = g.find_pin(pin);
    QCHECK(p != nullptr);
    QCHECK(p->kind == PinKind::Input);
}
