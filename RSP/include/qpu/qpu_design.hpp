#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <queue>
#include <iostream>
#include "hardware/observability.hpp"
#include "qhal/IQuantumBackend.hpp"

namespace rvdlrsp::qpu
{

    // ─────────────────────────────────────────────────────────────
    // 量子处理器(QPU)设计
    //
    // 提供拓扑感知的门路由(最近邻约束 + SWAP 插入)、退相干误差
    // 估计、标准程序(Bell/GHZ)生成,以及把程序编译到量子后端。
    // ─────────────────────────────────────────────────────────────

    enum class QubitTopology
    {
        Linear,
        Grid,
        AllToAll
    };

    struct QPUSpec
    {
        std::string name;
        size_t num_qubits = 8;
        QubitTopology topology = QubitTopology::Linear;
        double coherence_time_us = 100.0; // 退相干时间 T2
        double gate_time_us = 0.1;        // 单门耗时(用于退相干误差)
    };

    // 门序列程序
    struct QPLProgram
    {
        std::string name;
        struct GateOp
        {
            std::string name;  // "H" / "X" / "RZ" / "CNOT" / "SWAP"
            int target = -1;
            int control = -1;  // CNOT/SWAP 的另一个 qubit
            double angle = 0.0; // RZ 角度
        };
        std::vector<GateOp> ops;
    };

    class QPUDesigner
    {
    public:
        // Grid 拓扑的列宽(近似正方形网格)
        static size_t grid_width(size_t num_qubits)
        {
            if (num_qubits == 0)
                return 1;
            return static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(num_qubits))));
        }

        // 设计 QPU(指定拓扑)
        static QPUSpec design_qpu(const std::string &name, size_t qubits,
                                  QubitTopology topology = QubitTopology::Grid)
        {
            QPUSpec s{name, qubits, topology, 100.0, 0.1};
            RVDLRSP_INFO("qpu") << "Designed QPU '" << name << "' (" << qubits
                                 << " qubits, topology=" << topology_name(topology) << ").";
            return s;
        }

        // 判断两个 qubit 是否物理相邻
        static bool adjacent(const QPUSpec &spec, size_t a, size_t b)
        {
            if (a == b)
                return true;
            if (a >= spec.num_qubits || b >= spec.num_qubits)
                return false;
            switch (spec.topology)
            {
            case QubitTopology::AllToAll:
                return true;
            case QubitTopology::Linear:
                return (a + 1 == b) || (b + 1 == a);
            case QubitTopology::Grid:
            {
                size_t w = grid_width(spec.num_qubits);
                size_t ra = a / w, ca = a % w;
                size_t rb = b / w, cb = b % w;
                bool same_row = (ra == rb) && (ca + 1 == cb || cb + 1 == ca);
                bool same_col = (ca == cb) && (ra + 1 == rb || rb + 1 == ra);
                return same_row || same_col;
            }
            }
            return false;
        }

        // 最近邻路由:把 CNOT(control, target) 分解为相邻 CNOT + SWAP 序列。
        // 通过 BFS 找 control→target 最短路径,沿路径 SWAP 把 control 移到
        // target 旁,再执行相邻 CNOT。返回的门序列在任意拓扑下均可物理执行。
        static QPLProgram route_cnot(const QPUSpec &spec, int control, int target)
        {
            QPLProgram p;
            p.name = "cnot_routed";
            if (control == target)
                return p;
            if (control < 0 || target < 0 ||
                control >= static_cast<int>(spec.num_qubits) ||
                target >= static_cast<int>(spec.num_qubits))
                return p;

            if (adjacent(spec, static_cast<size_t>(control), static_cast<size_t>(target)))
            {
                p.ops.push_back({"CNOT", target, control, 0.0});
                return p;
            }

            // BFS 最短路径 control → target
            std::vector<int> prev(spec.num_qubits, -1);
            std::vector<int> frontier;
            frontier.push_back(control);
            prev[control] = control;
            for (size_t head = 0; head < frontier.size(); ++head)
            {
                int cur = frontier[head];
                if (cur == target)
                    break;
                for (size_t nb = 0; nb < spec.num_qubits; ++nb)
                {
                    if (static_cast<int>(nb) == cur)
                        continue;
                    if (prev[nb] != -1)
                        continue;
                    if (adjacent(spec, static_cast<size_t>(cur), nb))
                    {
                        prev[nb] = cur;
                        frontier.push_back(static_cast<int>(nb));
                    }
                }
            }
            if (prev[target] == -1)
                return p; // 拓扑不连通

            // 重建路径 control → ... → target
            std::vector<int> path;
            for (int cur = target; cur != control; cur = prev[cur])
                path.push_back(cur);
            path.push_back(control);
            std::reverse(path.begin(), path.end());

            // 沿路径 SWAP,把 control 移到 target 旁(path[path.size()-2])
            int pos = control;
            for (size_t i = 1; i + 1 < path.size(); ++i)
            {
                int next = path[i];
                p.ops.push_back({"SWAP", next, pos, 0.0}); // SWAP(pos, next)
                pos = next;
            }
            p.ops.push_back({"CNOT", target, pos, 0.0}); // pos 与 target 相邻
            return p;
        }

        // 退相干误差估计:累积 Σ(gate_time/T2 × 涉及 qubit 数)
        static double coherence_error(const QPUSpec &spec, const QPLProgram &prog)
        {
            if (spec.coherence_time_us <= 0.0)
                return 0.0;
            double total = 0.0;
            for (const auto &op : prog.ops)
            {
                int involved = (op.control >= 0) ? 2 : 1;
                total += spec.gate_time_us / spec.coherence_time_us * involved;
            }
            return total;
        }

        // 生成标准量子程序(Bell 态:CNOT 后 H,制备最大纠缠)
        static QPLProgram design_qpl(const std::string &name, size_t qubits = 2)
        {
            QPLProgram p;
            p.name = name.empty() ? "bell" : name;
            if (qubits >= 2)
            {
                p.ops.push_back({"H", 0, -1, 0.0});
                p.ops.push_back({"CNOT", 1, 0, 0.0});
            }
            return p;
        }

        // 把程序编译执行到量子后端
        static void execute(const QPLProgram &prog, qhal::IQuantumBackend &backend)
        {
            for (const auto &op : prog.ops)
            {
                if (op.name == "H")
                    backend.apply_h(static_cast<size_t>(op.target));
                else if (op.name == "X")
                    backend.apply_x(static_cast<size_t>(op.target));
                else if (op.name == "RZ")
                    backend.apply_rz(static_cast<size_t>(op.target), op.angle);
                else if (op.name == "CNOT")
                    backend.apply_cnot(static_cast<size_t>(op.control),
                                       static_cast<size_t>(op.target));
                else if (op.name == "SWAP")
                    backend.apply_swap(static_cast<size_t>(op.target),
                                       static_cast<size_t>(op.control));
            }
        }

        static const char *topology_name(QubitTopology t)
        {
            switch (t)
            {
            case QubitTopology::Linear: return "Linear";
            case QubitTopology::Grid: return "Grid";
            case QubitTopology::AllToAll: return "AllToAll";
            }
            return "Unknown";
        }
    };
}