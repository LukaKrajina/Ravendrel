#pragma once
#include <cmath>
#include <cstddef>
#include <vector>
#include <algorithm>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

namespace qhal
{
    enum class GateType
    {
        X,
        Y,
        Z,
        H,
        S,
        T,
        Rx,
        Ry,
        Rz,
        CNOT,
        Toffoli,
        SWAP,
        Braid,
        QFT,
        Measure
    };

    inline double pauli_commutation_phase(GateType a, GateType b)
    {
        auto is_pauli = [](GateType g)
        {
            return g == GateType::X || g == GateType::Y || g == GateType::Z;
        };
        if (is_pauli(a) && is_pauli(b) && a != b)
            return M_PI;
        return 0.0;
    }

    inline bool gates_commute(GateType a, GateType b)
    {
        return std::fabs(pauli_commutation_phase(a, b)) < 1e-9;
    }

    struct ScheduledGate
    {
        GateType type = GateType::X;
        size_t target = 0;
        size_t control = 0;
        double angle = 0.0;
    };

    inline bool gates_commute_scoped(const ScheduledGate &a, const ScheduledGate &b)
    {
        auto is_two_qubit = [](const ScheduledGate &g)
        {
            return g.type == GateType::CNOT || g.type == GateType::SWAP || g.type == GateType::Braid;
        };

        auto touches = [&](const ScheduledGate &g, size_t q) -> bool
        {
            if (is_two_qubit(g))
                return g.target == q || g.control == q;
            return g.target == q;
        };

        bool overlap = touches(b, a.target) || touches(a, b.target);
        if (is_two_qubit(a))
            overlap = overlap || touches(b, a.control);
        if (is_two_qubit(b))
            overlap = overlap || touches(a, b.control);

        if (!overlap)
            return true;
        return gates_commute(a.type, b.type);
    }

    inline std::vector<size_t> pruned_bidirectional_prune(const std::vector<size_t>& degrees)
    {
        size_t total = 0;
        for (size_t d : degrees) total += d;
        size_t target = (3 * total) / 4;

        std::vector<size_t> idx(degrees.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return degrees[a] > degrees[b]; });

        std::vector<size_t> high;
        size_t acc = 0;
        for (size_t i : idx) {
            if (acc >= target && !high.empty()) break;
            high.push_back(i);
            acc += degrees[i];
        }
        return high;
    }
}