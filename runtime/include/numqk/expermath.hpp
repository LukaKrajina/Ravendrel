#pragma once


#include "nqcmath.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace numqk
{
    namespace expermath
    {
        using Complex = std::complex<double>;
        using nqcmath::MatrixX;


        inline std::vector<uint8_t> index_to_bits(uint64_t index, size_t nqubits)
        {
            std::vector<uint8_t> bits(nqubits, 0);
            for (size_t k = 0; k < nqubits; ++k)
                bits[k] = static_cast<uint8_t>((index >> (nqubits - 1 - k)) & 1ull);
            return bits;
        }

        inline uint64_t bits_to_index(const std::vector<uint8_t> &bits)
        {
            uint64_t idx = 0;
            for (uint8_t b : bits)
                idx = (idx << 1) | static_cast<uint64_t>(b & 1);
            return idx;
        }


        inline std::vector<double> hermitian_eigenvalues(MatrixX<Complex> A,
                                                         int max_sweeps = 100,
                                                         double tol = 1e-14)
        {
            const size_t n = A.rows();
            if (n != A.cols())
                throw std::invalid_argument("expermath: hermitian_eigenvalues needs a square matrix");
            std::vector<double> vals(n, 0.0);
            if (n == 0)
                return vals;

            for (int sweep = 0; sweep < max_sweeps; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += std::norm(A(p, q));
                if (off < tol)
                    break;

                for (size_t p = 0; p < n; ++p)
                {
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const Complex z = A(p, q);
                        const double az = std::abs(z);
                        if (az < 1e-300)
                            continue;

                        const double phi = std::arg(z);
                        const Complex ep(std::cos(phi), std::sin(phi));
                        A(p, q) *= ep;
                        A(q, p) *= std::conj(ep);

                        const double a = A(p, p).real();
                        const double d = A(q, q).real();
                        const double b = A(p, q).real();
                        double t = 0.0;
                        if (std::abs(b) > 1e-300)
                        {
                            const double tau = (d - a) / (2.0 * b);
                            const double abs_tau = std::abs(tau);
                            t = (tau >= 0.0 ? 1.0 : -1.0) / (abs_tau + std::sqrt(1.0 + abs_tau * abs_tau));
                        }
                        const double c = 1.0 / std::sqrt(1.0 + t * t);
                        const double s = t * c;

                        for (size_t k = 0; k < n; ++k)
                        {
                            if (k == p || k == q)
                                continue;
                            const Complex akp = A(k, p);
                            const Complex akq = A(k, q);
                            A(k, p) = c * akp - s * akq;
                            A(p, k) = std::conj(A(k, p));
                            A(k, q) = s * akp + c * akq;
                            A(q, k) = std::conj(A(k, q));
                        }
                        A(p, p) = Complex(c * c * a - 2.0 * s * c * b + s * s * d, 0.0);
                        A(q, q) = Complex(s * s * a + 2.0 * s * c * b + c * c * d, 0.0);
                        A(p, q) = Complex(0.0, 0.0);
                        A(q, p) = Complex(0.0, 0.0);
                    }
                }
            }
            for (size_t i = 0; i < n; ++i)
                vals[i] = A(i, i).real();
            std::sort(vals.begin(), vals.end());
            return vals;
        }

        inline void hermitian_eigensystem(const MatrixX<Complex> &A, std::vector<double> &values,
                                          MatrixX<Complex> &vectors, int max_sweeps = 100,
                                          double tol = 1e-14)
        {
            const size_t n = A.rows();
            if (n != A.cols())
                throw std::invalid_argument("expermath: hermitian_eigensystem needs a square matrix");
            values.clear();
            vectors = MatrixX<Complex>(n, n, Complex(0.0, 0.0));
            for (size_t i = 0; i < n; ++i)
                vectors(i, i) = Complex(1.0, 0.0);
            if (n == 0)
                return;

            MatrixX<Complex> M(A);
            for (int sweep = 0; sweep < max_sweeps; ++sweep)
            {
                double off = 0.0;
                for (size_t p = 0; p < n; ++p)
                    for (size_t q = p + 1; q < n; ++q)
                        off += std::norm(M(p, q));
                if (off < tol)
                    break;

                for (size_t p = 0; p < n; ++p)
                {
                    for (size_t q = p + 1; q < n; ++q)
                    {
                        const Complex z = M(p, q);
                        const double az = std::abs(z);
                        if (az < 1e-300)
                            continue;

                        const double phi = std::arg(z);
                        const Complex ep(std::cos(phi), std::sin(phi));
                        M(p, q) *= ep;
                        M(q, p) *= std::conj(ep);

                        const double a = M(p, p).real();
                        const double d = M(q, q).real();
                        const double b = M(p, q).real();
                        double t = 0.0;
                        if (std::abs(b) > 1e-300)
                        {
                            const double tau = (d - a) / (2.0 * b);
                            const double abs_tau = std::abs(tau);
                            t = (tau >= 0.0 ? 1.0 : -1.0) / (abs_tau + std::sqrt(1.0 + abs_tau * abs_tau));
                        }
                        const double c = 1.0 / std::sqrt(1.0 + t * t);
                        const double s = t * c;

                        for (size_t k = 0; k < n; ++k)
                        {
                            if (k == p || k == q)
                                continue;
                            const Complex mkp = M(k, p);
                            const Complex mkq = M(k, q);
                            M(k, p) = c * mkp - s * mkq;
                            M(p, k) = std::conj(M(k, p));
                            M(k, q) = s * mkp + c * mkq;
                            M(q, k) = std::conj(M(k, q));
                        }
                        M(p, p) = Complex(c * c * a - 2.0 * s * c * b + s * s * d, 0.0);
                        M(q, q) = Complex(s * s * a + 2.0 * s * c * b + c * c * d, 0.0);
                        M(p, q) = Complex(0.0, 0.0);
                        M(q, p) = Complex(0.0, 0.0);

                        for (size_t k = 0; k < n; ++k)
                        {
                            const Complex vkp = vectors(k, p);
                            const Complex vkq = vectors(k, q);
                            vectors(k, p) = c * vkp - s * vkq;
                            vectors(k, q) = ep * (s * vkp + c * vkq);
                        }
                    }
                }
            }

            std::vector<size_t> order(n);
            for (size_t i = 0; i < n; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](size_t x, size_t y) { return M(x, x).real() < M(y, y).real(); });

            values.resize(n);
            MatrixX<Complex> V(n, n);
            for (size_t j = 0; j < n; ++j)
            {
                values[j] = M(order[j], order[j]).real();
                for (size_t k = 0; k < n; ++k)
                    V(k, j) = vectors(k, order[j]);
            }
            vectors = V;
        }


        inline MatrixX<Complex> partial_trace(const std::vector<Complex> &psi, size_t nqubits,
                                               const std::vector<int> &keep)
        {
            const size_t dim = static_cast<size_t>(1) << nqubits;
            if (psi.size() != dim)
                throw std::invalid_argument("expermath: partial_trace amplitude count mismatch");

            std::vector<size_t> keep_pos;
            std::vector<size_t> trace_pos;
            for (size_t k = 0; k < nqubits; ++k)
            {
                const bool in_keep = std::find(keep.begin(), keep.end(), static_cast<int>(k)) != keep.end();
                if (in_keep)
                    keep_pos.push_back(nqubits - 1 - k);
                else
                    trace_pos.push_back(nqubits - 1 - k);
            }
            const size_t dk = static_cast<size_t>(1) << keep_pos.size();
            const size_t dt = static_cast<size_t>(1) << trace_pos.size();

            MatrixX<Complex> rho(dk, dk);

            std::vector<uint64_t> base(dk, 0);
            for (size_t i = 0; i < dk; ++i)
            {
                uint64_t idx = 0;
                uint64_t bits = i;
                for (size_t p = 0; p < keep_pos.size(); ++p)
                {
                    const uint64_t b = (bits >> (keep_pos.size() - 1 - p)) & 1ull;
                    idx |= b << keep_pos[p];
                }
                base[i] = idx;
            }

            const size_t nthreads = static_cast<size_t>(nqcmath::Parallel::get_threads());
            const size_t block = (dk + nthreads * 4 - 1) / (nthreads * 4);
            std::vector<std::vector<Complex>> partial_i(block == 0 ? 1 : block);

            nqcmath::Parallel::parallel_for(dk, [&](size_t i)
                                             {
                const size_t b = std::min(i / std::max<size_t>(block, 1), partial_i.size() - 1);
                for (size_t j = 0; j < dk; ++j)
                {
                    Complex acc(0.0, 0.0);
                    const uint64_t bi = base[i];
                    const uint64_t bj = base[j];
                    for (size_t t = 0; t < dt; ++t)
                    {
                        uint64_t idx_i = bi;
                        uint64_t idx_j = bj;
                        uint64_t tt = t;
                        for (size_t p = 0; p < trace_pos.size(); ++p)
                        {
                            const uint64_t bit = (tt >> (trace_pos.size() - 1 - p)) & 1ull;
                            idx_i |= bit << trace_pos[p];
                            idx_j |= bit << trace_pos[p];
                        }
                        acc += psi[idx_i] * std::conj(psi[idx_j]);
                    }
                    rho(i, j) = acc;
                }
             });

            (void)partial_i;
            return rho;
        }

        inline MatrixX<Complex> density_matrix(const std::vector<Complex> &psi, size_t nqubits)
        {
            const size_t dim = static_cast<size_t>(1) << nqubits;
            if (psi.size() != dim)
                throw std::invalid_argument("expermath: density_matrix amplitude count mismatch");
            MatrixX<Complex> rho(dim, dim);
            for (size_t i = 0; i < dim; ++i)
                for (size_t j = 0; j < dim; ++j)
                    rho(i, j) = psi[i] * std::conj(psi[j]);
            return rho;
        }


        inline double von_neumann_entropy(const MatrixX<Complex> &rho, double tol = 1e-12)
        {
            const std::vector<double> ev = hermitian_eigenvalues(rho);
            double s = 0.0;
            for (double l : ev)
            {
                if (l <= tol)
                    continue;
                s -= l * std::log(l);
            }
            return s;
        }

        inline double renyi_entropy(const MatrixX<Complex> &rho, double alpha)
        {
            if (std::abs(alpha - 1.0) < 1e-12)
                return von_neumann_entropy(rho);
            const std::vector<double> ev = hermitian_eigenvalues(rho);
            double tr = 0.0;
            for (double l : ev)
                if (l > 1e-12)
                    tr += std::pow(l, alpha);
            if (tr <= 0.0)
                return 0.0;
            return std::log(tr) / (1.0 - alpha);
        }

        inline double purity(const MatrixX<Complex> &rho)
        {
            double p = 0.0;
            for (size_t i = 0; i < rho.rows(); ++i)
                for (size_t j = 0; j < rho.cols(); ++j)
                    p += std::real(rho(i, j) * std::conj(rho(j, i)));
            return p;
        }

        inline double linear_entropy(const MatrixX<Complex> &rho)
        {
            return 1.0 - purity(rho);
        }

        inline double entanglement_entropy(const std::vector<Complex> &psi, size_t nqubits,
                                           size_t cut)
        {
            if (cut == 0 || cut > nqubits)
                return 0.0;
            std::vector<int> keep(cut);
            for (size_t k = 0; k < cut; ++k)
                keep[k] = static_cast<int>(k);
            const MatrixX<Complex> rho_a = partial_trace(psi, nqubits, keep);
            return von_neumann_entropy(rho_a);
        }

        inline double renyi_entanglement_entropy(const std::vector<Complex> &psi, size_t nqubits,
                                                 size_t cut, double alpha)
        {
            if (cut == 0 || cut > nqubits)
                return 0.0;
            std::vector<int> keep(cut);
            for (size_t k = 0; k < cut; ++k)
                keep[k] = static_cast<int>(k);
            const MatrixX<Complex> rho_a = partial_trace(psi, nqubits, keep);
            return renyi_entropy(rho_a, alpha);
        }


        inline Complex inner(const std::vector<Complex> &a, const std::vector<Complex> &b)
        {
            if (a.size() != b.size())
                throw std::invalid_argument("expermath: inner size mismatch");
            Complex acc(0.0, 0.0);
            for (size_t i = 0; i < a.size(); ++i)
                acc += std::conj(a[i]) * b[i];
            return acc;
        }

        inline double overlap_fidelity(const std::vector<Complex> &a,
                                        const std::vector<Complex> &b)
        {
            const Complex o = inner(a, b);
            return std::norm(o);
        }

        inline double fidelity(const std::vector<Complex> &a, const std::vector<Complex> &b)
        {
            return overlap_fidelity(a, b);
        }

        inline double trace_distance(const std::vector<Complex> &a, const std::vector<Complex> &b)
        {
            const double f = overlap_fidelity(a, b);
            const double d = 1.0 - f;
            return d <= 0.0 ? 0.0 : std::sqrt(d);
        }


        inline std::vector<Complex> apply_pauli(const std::vector<Complex> &psi, size_t nqubits,
                                                const std::vector<int> &spec)
        {
            if (spec.size() != nqubits)
                throw std::invalid_argument("expermath: pauli spec length mismatch");
            const size_t dim = static_cast<size_t>(1) << nqubits;
            if (psi.size() != dim)
                throw std::invalid_argument("expermath: pauli amplitude count mismatch");

            std::vector<Complex> out(dim);
            for (size_t i = 0; i < dim; ++i)
            {
                uint64_t flipped = i;
                Complex phase(1.0, 0.0);
                for (size_t k = 0; k < nqubits; ++k)
                {
                    const int p = spec[k];
                    if (p == 0)
                        continue;
                    const size_t bit = nqubits - 1 - k;
                    const bool b = ((i >> bit) & 1ull) != 0;
                    if (p == 1)
                    {
                        flipped ^= (1ull << bit);
                    }
                    else if (p == 2)
                    {
                        flipped ^= (1ull << bit);
                        phase *= (b ? Complex(0.0, -1.0) : Complex(0.0, 1.0));
                    }
                    else if (p == 3)
                    {
                        phase *= (b ? Complex(-1.0, 0.0) : Complex(1.0, 0.0));
                    }
                }
                out[flipped] = phase * psi[i];
            }
            return out;
        }

        inline Complex pauli_expectation(const std::vector<Complex> &psi, size_t nqubits,
                                         const std::vector<int> &spec)
        {
            const std::vector<Complex> ppsi = apply_pauli(psi, nqubits, spec);
            return inner(psi, ppsi);
        }

        inline double hamiltonian_expectation(const std::vector<Complex> &psi, size_t nqubits,
                                               const std::vector<std::vector<int>> &paulis,
                                               const std::vector<double> &coeffs)
        {
            if (paulis.size() != coeffs.size())
                throw std::invalid_argument("expermath: pauli/coefficient count mismatch");
            double e = 0.0;
            for (size_t k = 0; k < paulis.size(); ++k)
                e += coeffs[k] * pauli_expectation(psi, nqubits, paulis[k]).real();
            return e;
        }

        inline double quantum_fisher_information(const std::vector<Complex> &psi, size_t nqubits,
                                                  const std::vector<std::vector<int>> &generators,
                                                  const std::vector<double> &coeffs)
        {
            if (generators.size() != coeffs.size())
                throw std::invalid_argument("expermath: generator/coefficient count mismatch");

            const size_t dim = static_cast<size_t>(1) << nqubits;
            std::vector<Complex> gpsi(dim, Complex(0.0, 0.0));
            for (size_t k = 0; k < generators.size(); ++k)
            {
                const std::vector<Complex> ppsi = apply_pauli(psi, nqubits, generators[k]);
                for (size_t i = 0; i < dim; ++i)
                    gpsi[i] += coeffs[k] * ppsi[i];
            }
            const Complex mg = inner(psi, gpsi);
            const Complex gg = inner(gpsi, gpsi);
            const double var = gg.real() - std::norm(mg);
            return var > 0.0 ? 4.0 * var : 0.0;
        }


        struct GateOp
        {
            enum class Kind
            {
                H,
                X,
                Y,
                Z,
                S,
                T,
                RX,
                RY,
                RZ,
                CNOT,
                CZ,
                SWAP,
                CRZ,
                CPHASE,
                CCX
            };
            Kind kind = Kind::H;
            int q0 = -1;
            int q1 = -1;
            int q2 = -1;
            double angle = 0.0;
        };

        using Circuit = std::vector<GateOp>;

        inline Circuit qft_circuit(size_t nqubits, bool inverse = false)
        {
            Circuit c;
            if (nqubits == 0)
                return c;

            for (size_t j = 0; j < nqubits; ++j)
            {
                c.push_back({GateOp::Kind::H, static_cast<int>(j), -1, -1, 0.0});
                for (size_t m = j + 1; m < nqubits; ++m)
                {
                    const double ang =
                        2.0 * M_PI / std::pow(2.0, static_cast<double>(m - j + 1));
                    c.push_back({GateOp::Kind::CPHASE, static_cast<int>(j), static_cast<int>(m), -1, ang});
                }
            }
            for (size_t j = 0; j < nqubits / 2; ++j)
                c.push_back({GateOp::Kind::SWAP, static_cast<int>(j),
                             static_cast<int>(nqubits - 1 - j), -1, 0.0});

            if (inverse)
            {
                std::reverse(c.begin(), c.end());
                for (GateOp &g : c)
                    g.angle = -g.angle;
            }
            return c;
        }

        inline Circuit trotter_circuit(const std::vector<std::vector<int>> &paulis,
                                       const std::vector<double> &coeffs, double time = 1.0,
                                       int order = 1, int repetitions = 1)
        {
            if (paulis.size() != coeffs.size())
                throw std::invalid_argument("expermath: pauli/coefficient count mismatch");
            if (order != 1 && order != 2)
                throw std::invalid_argument("expermath: trotter order must be 1 or 2");

            Circuit out;
            const double half = (order == 2) ? 0.5 : 1.0;
            for (int rep = 0; rep < repetitions; ++rep)
            {
                for (int half_pass = 0; half_pass < (order == 2 ? 2 : 1); ++half_pass)
                {
                    for (size_t k = 0; k < paulis.size(); ++k)
                    {
                        const double theta = coeffs[k] * time * half;
                        for (size_t b = 0; b < paulis[k].size(); ++b)
                        {
                            const int p = paulis[k][b];
                            if (p == 0)
                                continue;
                            const double ang = 2.0 * theta;
                            switch (p)
                            {
                            case 1:
                                out.push_back({GateOp::Kind::RX, static_cast<int>(b), -1, -1, ang});
                                break;
                            case 2:
                                out.push_back({GateOp::Kind::RY, static_cast<int>(b), -1, -1, ang});
                                break;
                            case 3:
                                out.push_back({GateOp::Kind::RZ, static_cast<int>(b), -1, -1, ang});
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
            }
            return out;
        }

        inline Circuit amplitude_encode_circuit(size_t nqubits, uint64_t index)
        {
            Circuit c;
            for (size_t k = 0; k < nqubits; ++k)
            {
                const size_t bit = nqubits - 1 - k;
                if ((index >> bit) & 1ull)
                    c.push_back({GateOp::Kind::RY, static_cast<int>(k), -1, -1, M_PI});
            }
            return c;
        }
    }
}