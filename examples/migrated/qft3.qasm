// OpenQASM 3.0 三比特 QFT 示例（含 for 循环 + 自定义门）
// 迁移：rvdl migrate examples/migrated/qft3.qasm
OPENQASM 3.0;
qubit[3] q;
bit[3] c;
h q[0];
h q[1];
h q[2];
for i in [0:3] {
    rz(0.5) q[i];
}
c = measure q;