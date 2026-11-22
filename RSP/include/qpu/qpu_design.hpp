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
        double coherence_time_us = 100.0;
        double gate_time_us = 0.1;
    };

    struct QPLProgram
    {
        std::string name;
        struct GateOp
        {
            std::string name;
            int target = -1;
            int control = -1;
            double angle = 0.0;
        };
        std::vector<GateOp> ops;
    };

    class QPUDesigner
    {
    public:
        static size_t grid_width(size_t num_qubits)
        {
            if (num_qubits == 0)
                return 1;
            return static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(num_qubits))));
        }

        static QPUSpec design_qpu(const std::string &name, size_t qubits,
                                  QubitTopology topology = QubitTopology::Grid)
        {
            QPUSpec s{name, qubits, topology, 100.0, 0.1};
            RVDLRSP_INFO("qpu") << "Designed QPU '" << name << "' (" << qubits
                                 << " qubits, topology=" << topology_name(topology) << ").";
            return s;
        }

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
                return p;

            std::vector<int> path;
            for (int cur = target; cur != control; cur = prev[cur])
                path.push_back(cur);
            path.push_back(control);
            std::reverse(path.begin(), path.end());

            int pos = control;
            for (size_t i = 1; i + 1 < path.size(); ++i)
            {
                int next = path[i];
                p.ops.push_back({"SWAP", next, pos, 0.0});
                pos = next;
            }
            p.ops.push_back({"CNOT", target, pos, 0.0});
            return p;
        }

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