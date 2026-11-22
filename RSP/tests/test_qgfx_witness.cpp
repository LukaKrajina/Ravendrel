#include "test_framework.hpp"
#include "qhal/QuantumState.hpp"

#include <cmath>
#include <vector>

using namespace qhal;

namespace
{
    static const double kSqrtHalf = std::sqrt(0.5);
    static const double kInvSqrt3 = 1.0 / std::sqrt(3.0);

    inline qcomplex P(double r, double i) { return qcomplex(r, i); }

    QuantumFrame ghz3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(0, 0),
                          P(0, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0)}, 3);
        return f;
    }

    QuantumFrame w3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(0, 0), P(kInvSqrt3, 0), P(kInvSqrt3, 0), P(0, 0),
                          P(kInvSqrt3, 0), P(0, 0), P(0, 0), P(0, 0)}, 3);
        return f;
    }

    QuantumFrame product3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(kSqrtHalf, 0), P(0, 0),
                          P(0, 0), P(0, 0), P(0, 0), P(0, 0)}, 3);
        return f;
    }

    QuantumFrame biseparable3()
    {
        QuantumFrame f;
        f.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0),
                          P(0, 0), P(0, 0), P(0, 0), P(0, 0)}, 3);
        return f;
    }

    bool all_marginals_mixed(const QuantumFrame &f, double thresh = 0.5)
    {
        for (uint32_t q = 0; q < f.num_qubits(); ++q)
            if (f.von_neumann_entropy(q, 1) <= thresh)
                return false;
        return true;
    }

    bool has_separable_bipartition(const QuantumFrame &f)
    {
        for (uint32_t q = 0; q < f.num_qubits(); ++q)
            if (f.separable_across(q, 1))
                return true;
        return false;
    }
}


QTEST(genuine_sufficient_witness) {
    {
        const QuantumFrame g = ghz3();
        QCHECK(all_marginals_mixed(g));
        QCHECK(!has_separable_bipartition(g));
    }
    {
        const QuantumFrame w = w3();
        QCHECK(all_marginals_mixed(w));
        QCHECK(!has_separable_bipartition(w));
    }
    {
        const QuantumFrame p = product3();
        QCHECK(!all_marginals_mixed(p));
        QCHECK(has_separable_bipartition(p));
    }
}

QTEST(biseparable_is_not_genuine) {
    const QuantumFrame b = biseparable3();
    QCHECK(!all_marginals_mixed(b));
    QCHECK(has_separable_bipartition(b));
}

QTEST(gvh_vidal_witness_is_sufficient_not_necessary) {
    const QuantumFrame b = biseparable3();
    QCHECK_NEAR(b.von_neumann_entropy(2, 1), 0.0, 1e-12);
    QCHECK(b.von_neumann_entropy(0, 1) > 0.5);
    QCHECK(!b.separable_across(0, 1));
    QCHECK(!b.separable_across(1, 1));
}

QTEST(l1_separability_is_necessary_and_sufficient) {
    {
        const QuantumFrame g = ghz3();
        for (uint32_t q = 0; q < 3; ++q)
            QCHECK(!g.separable_across(q, 1));
    }
    {
        const QuantumFrame b = biseparable3();
        QCHECK(b.separable_across(2, 1));
        QCHECK(!b.separable_across(0, 1));
        QCHECK(!b.separable_across(1, 1));
    }
    {
        const QuantumFrame p = product3();
        for (uint32_t q = 0; q < 3; ++q)
            QCHECK(p.separable_across(q, 1));
    }
}

QTEST(two_qubit_concurrence_is_sufficient_witness) {
    QuantumFrame bell;
    bell.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0)}, 2);
    QCHECK(bell.concurrence() > 0.0);
    QCHECK(!bell.separable_across(0, 1));
    QCHECK(bell.entanglement_exists(0, 1));

    QuantumFrame prod;
    prod.set_amplitudes({P(kSqrtHalf, 0), P(kSqrtHalf, 0), P(0, 0), P(0, 0)}, 2);
    QCHECK(prod.concurrence() == 0.0);
    QCHECK(prod.separable_across(0, 1));
    QCHECK(!prod.entanglement_exists(0, 1));
}

QTEST(pair_strength_only_valid_for_two_qubits) {
    const QuantumFrame g = ghz3();
    QCHECK(g.num_qubits() == 3);
    QCHECK_NEAR(g.pair_entanglement(0, 1), 0.0, 1e-12);
    QCHECK(g.entanglement_exists(0, 1));

    const QuantumFrame b = biseparable3();
    QCHECK_NEAR(b.pair_entanglement(0, 1), 0.0, 1e-12);
    QCHECK(b.entanglement_exists(0, 0));
    QCHECK(!b.entanglement_exists(0, 1));
    QCHECK(b.separable_across(2, 1));

    QuantumFrame bell;
    bell.set_amplitudes({P(kSqrtHalf, 0), P(0, 0), P(0, 0), P(kSqrtHalf, 0)}, 2);
    QCHECK_NEAR(bell.pair_entanglement(0, 1), bell.concurrence(), 1e-12);
}