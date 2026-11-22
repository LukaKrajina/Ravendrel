// Q#（Microsoft）Bell 态示例
// 迁移：rvdl migrate examples/migrated/bell.qs
namespace Quantum.Bell {
    operation Bell() : Result[] {
        use (q0, q1) = (Qubit(), Qubit());
        H(q0);
        CNOT(q0, q1);
        let r0 = M(q0);
        let r1 = M(q1);
        Reset(q0);
        Reset(q1);
        return [r0, r1];
    }
}