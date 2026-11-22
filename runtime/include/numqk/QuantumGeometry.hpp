#pragma once
#include "ComplexTensor.hpp"
#include <vector>
#include <cmath>
#include <stdexcept>

namespace numqk
{

    inline CTensor pauli_matrix(int which)
    {
        CTensor M({2, 2}, false);
        switch (which)
        {
        case 0:
            M.data()[0] = Complex(1, 0);
            M.data()[3] = Complex(1, 0);
            break;
        case 1:
            M.data()[1] = Complex(1, 0);
            M.data()[2] = Complex(1, 0);
            break;
        case 2:
            M.data()[1] = Complex(0, -1);
            M.data()[2] = Complex(0, 1);
            break;
        case 3:
            M.data()[0] = Complex(1, 0);
            M.data()[3] = Complex(-1, 0);
            break;
        default:
            throw std::invalid_argument("pauli_matrix: index must be 0..3.");
        }
        return M;
    }

    inline CTensor pauli_i() { return pauli_matrix(0); }
    inline CTensor pauli_x() { return pauli_matrix(1); }
    inline CTensor pauli_y() { return pauli_matrix(2); }
    inline CTensor pauli_z() { return pauli_matrix(3); }

    inline CTensor n_qubit_pauli(const std::vector<int> &spec)
    {
        if (spec.empty())
            throw std::invalid_argument("n_qubit_pauli: empty spec.");
        CTensor P = pauli_matrix(spec[0]);
        for (size_t i = 1; i < spec.size(); ++i)
            P = kron(P, pauli_matrix(spec[i]));
        return P;
    }

    inline CTensor commutator(const CTensor &A, const CTensor &B)
    {
        return A.matmul(B).sub(B.matmul(A));
    }

    inline Complex frobenius(const CTensor &A, const CTensor &B)
    {
        if (A.size() != B.size())
            throw std::invalid_argument("frobenius: size mismatch.");
        Complex s(0, 0);
        for (size_t i = 0; i < A.size(); ++i)
            s += std::conj(A.data()[i]) * B.data()[i];
        return s;
    }

    inline double fro_norm(const CTensor &M) { return std::sqrt(frobenius(M, M).real()); }

    inline CTensor deep_copy(const CTensor &m)
    {
        CTensor c(m.get_shape(), false);
        for (size_t i = 0; i < m.size(); ++i)
            c.data()[i] = m.data()[i];
        return c;
    }

    inline CTensor orthogonalize(const CTensor &m, const std::vector<CTensor> &basis)
    {
        CTensor v = deep_copy(m);
        for (const auto &b : basis)
        {
            Complex c = frobenius(b, v);
            for (size_t i = 0; i < v.size(); ++i)
                v.data()[i] -= c * b.data()[i];
        }
        return v;
    }

    inline std::vector<CTensor> lie_closure(const std::vector<CTensor> &generators,
                                             size_t max_dim = 256, double tol = 1e-10)
    {
        std::vector<CTensor> basis;
        for (const auto &g : generators)
        {
            CTensor v = orthogonalize(g, basis);
            double nrm = fro_norm(v);
            if (nrm > tol)
            {
                for (size_t i = 0; i < v.size(); ++i)
                    v.data()[i] /= nrm;
                basis.push_back(v);
            }
        }

        bool changed = true;
        while (changed && basis.size() < max_dim)
        {
            changed = false;
            const size_t sz = basis.size();
            for (size_t i = 0; i < sz; ++i)
            {
                for (size_t j = 0; j < sz; ++j)
                {
                    CTensor C = commutator(basis[i], basis[j]);
                    CTensor v = orthogonalize(C, basis);
                    double nrm = fro_norm(v);
                    if (nrm > tol)
                    {
                        for (size_t k = 0; k < v.size(); ++k)
                            v.data()[k] /= nrm;
                        basis.push_back(v);
                        changed = true;
                        if (basis.size() >= max_dim)
                            break;
                    }
                }
                if (basis.size() >= max_dim)
                    break;
            }
        }
        return basis;
    }

    inline size_t dla_dimension(const std::vector<CTensor> &generators, size_t max_dim = 256)
    {
        return lie_closure(generators, max_dim).size();
    }

    inline Tensor<double> fubini_study(const std::vector<CTensor> &dpsi, const CTensor &psi)
    {
        const size_t n = dpsi.size();
        Tensor<double> G({n, n}, false);
        for (size_t i = 0; i < n; ++i)
        {
            Complex ai = inner(dpsi[i], psi);
            for (size_t j = 0; j < n; ++j)
            {
                Complex aj = inner(dpsi[j], psi);
                Complex g = inner(dpsi[i], dpsi[j]) - ai * std::conj(aj);
                G.data()[i * n + j] = g.real();
            }
        }
        return G;
    }

    inline Tensor<double> berry_curvature(const std::vector<CTensor> &dpsi, const CTensor &psi)
    {
        const size_t n = dpsi.size();
        Tensor<double> B({n, n}, false);
        for (size_t i = 0; i < n; ++i)
        {
            Complex ai = inner(dpsi[i], psi);
            for (size_t j = 0; j < n; ++j)
            {
                Complex aj = inner(dpsi[j], psi);
                Complex g = inner(dpsi[i], dpsi[j]) - ai * std::conj(aj);
                B.data()[i * n + j] = -2.0 * g.imag();
            }
        }
        return B;
    }

    inline Tensor<double> sym_pinv(const Tensor<double> &G, double tol = 1e-10)
    {
        const size_t n = G.get_shape()[0];
        std::vector<double> A(G.size());
        for (size_t i = 0; i < G.size(); ++i)
            A[i] = G.data()[i];

        std::vector<double> ev, V;
        jacobi_sym(A, n, ev, V);

        Tensor<double> Gp({n, n}, false);
        for (size_t k = 0; k < n; ++k)
        {
            const double inv = (ev[k] > tol) ? (1.0 / ev[k]) : 0.0;
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < n; ++j)
                    Gp.data()[i * n + j] += inv * V[i * n + k] * V[j * n + k];
        }
        return Gp;
    }

    inline Tensor<double> natural_gradient(const Tensor<double> &G, const Tensor<double> &grad)
    {
        const size_t n = grad.size();
        Tensor<double> Gp = sym_pinv(G);
        Tensor<double> d({n}, false);
        for (size_t i = 0; i < n; ++i)
        {
            double acc = 0.0;
            for (size_t j = 0; j < n; ++j)
                acc += Gp.data()[i * n + j] * grad.data()[j];
            d.data()[i] = acc;
        }
        return d;
    }

    inline CTensor apply_kraus(const CTensor &rho, const std::vector<CTensor> &Ks)
    {
        CTensor out(rho.get_shape(), false);
        for (const auto &K : Ks)
        {
            CTensor Kd = dagger(K);
            out = out.add(K.matmul(rho).matmul(Kd));
        }
        return out;
    }

    inline CTensor depolarizing(const CTensor &rho, double p)
    {
        const size_t d = rho.get_shape()[0];
        CTensor out(rho.get_shape(), false);
        const double c1 = 1.0 - p;
        for (size_t i = 0; i < rho.size(); ++i)
            out.data()[i] = c1 * rho.data()[i];
        const double c2 = p / static_cast<double>(d);
        for (size_t i = 0; i < d; ++i)
            out.data()[i * d + i] += c2;
        return out;
    }

    inline CTensor dephasing(const CTensor &rho, double p)
    {
        CTensor Z = pauli_z();
        CTensor zrz = Z.matmul(rho).matmul(Z);
        CTensor out(rho.get_shape(), false);
        for (size_t i = 0; i < rho.size(); ++i)
            out.data()[i] = (1.0 - p) * rho.data()[i] + p * zrz.data()[i];
        return out;
    }

    inline CTensor amplitude_damping(const CTensor &rho, double gamma)
    {
        CTensor K0({2, 2}, false), K1({2, 2}, false);
        K0.data()[0] = Complex(1, 0);
        K0.data()[3] = Complex(std::sqrt(1.0 - gamma), 0);
        K1.data()[1] = Complex(std::sqrt(gamma), 0);
        return apply_kraus(rho, {K0, K1});
    }

    inline double entanglement_entropy(const CTensor &psi, const std::vector<size_t> &dims,
                                       const std::vector<bool> &keep)
    {
        CTensor rA = partial_trace_pure(psi, dims, keep);
        return von_neumann_entropy(rA);
    }
}