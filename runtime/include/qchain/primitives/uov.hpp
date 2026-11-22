#pragma once
#include "../common.hpp"
#include "hash.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <vector>

namespace qchain::pqc
{

    using qchain::Byte;
    using qchain::Bytes;

    struct UovParams
    {
        static constexpr int O = 32;
        static constexpr int V = 48;
        static constexpr int N = O + V;
        static constexpr int QUAD_COUNT = N * (N + 1) / 2;
        static constexpr int POLY_BYTES = QUAD_COUNT + N + 1;
        static constexpr int PK_BYTES = O * POLY_BYTES;
        static constexpr int SK_BYTES = N * N + O * (1 + V + O * V + V * (V + 1) / 2);
    };

    namespace uov_detail
    {
        using P = UovParams;

        inline const std::array<uint8_t, 256> &gf_exp()
        {
            static const std::array<uint8_t, 256> exp = []()
            {
                std::array<uint8_t, 256> e{};
                int x = 1;
                for (int i = 0; i < 255; ++i)
                {
                    e[i] = static_cast<uint8_t>(x);
                    int xt = (x << 1) ^ ((x & 0x80) ? 0x11B : 0);
                    x = (xt ^ x) & 0xFF;
                }
                e[255] = e[0];
                return e;
            }();
            return exp;
        }
        inline const std::array<uint8_t, 256> &gf_log()
        {
            static const std::array<uint8_t, 256> log = []()
            {
                std::array<uint8_t, 256> l{};
                const auto &e = gf_exp();
                for (int i = 0; i < 255; ++i)
                    l[e[i]] = static_cast<uint8_t>(i);
                return l;
            }();
            return log;
        }
        inline uint8_t gf_mul(uint8_t a, uint8_t b)
        {
            if (a == 0 || b == 0)
                return 0;
            const auto &e = gf_exp();
            const auto &l = gf_log();
            return e[(static_cast<int>(l[a]) + l[b]) % 255];
        }
        inline uint8_t gf_inv(uint8_t a)
        {
            if (a == 0)
                return 0;
            const auto &e = gf_exp();
            const auto &l = gf_log();
            return e[255 - l[a]];
        }

        struct Poly
        {
            std::vector<uint8_t> quad;
            std::vector<uint8_t> linear;
            uint8_t constant = 0;

            uint8_t eval(const uint8_t x[P::N]) const
            {
                uint8_t acc = constant;
                size_t qi = 0;
                for (int i = 0; i < P::V; ++i)
                    for (int j = i; j < P::V; ++j)
                        acc ^= gf_mul(quad[qi++], gf_mul(x[P::O + i], x[P::O + j]));
                for (int i = 0; i < P::O; ++i)
                    for (int j = 0; j < P::V; ++j)
                        acc ^= gf_mul(quad[qi++], gf_mul(x[i], x[P::O + j]));
                for (int i = 0; i < P::V; ++i)
                    acc ^= gf_mul(linear[i], x[P::O + i]);
                return acc;
            }
        };

        inline size_t quad_size() { return P::O * P::V + P::V * (P::V + 1) / 2; }

    class UovSignature
    {
        using P = UovParams;

        struct CenterMap
        {
            std::array<uov_detail::Poly, P::O> polys;
        };

    public:
        static std::pair<Bytes, Bytes> keygen()
        {
            qchain::Csprng csprng;

            CenterMap F;
            const size_t qsz = uov_detail::quad_size();
            for (auto &poly : F.polys)
            {
                poly.quad.resize(qsz);
                poly.linear.resize(P::V);
                for (auto &c : poly.quad)
                    c = csprng.random_bytes(1)[0];
                for (auto &c : poly.linear)
                    c = csprng.random_bytes(1)[0];
                poly.constant = csprng.random_bytes(1)[0];
            }

            std::array<std::array<uint8_t, P::N>, P::N> S, S_inv;
            for (int i = 0; i < P::N; ++i)
                for (int j = 0; j < P::N; ++j)
                    S[i][j] = csprng.random_bytes(1)[0];
            if (!invert_matrix(S, S_inv))
                return keygen();

            Bytes pk = build_public_key(F, S);

            Bytes sk;
            for (int i = 0; i < P::N; ++i)
                for (int j = 0; j < P::N; ++j)
                    sk.push_back(S[i][j]);
            for (auto &poly : F.polys)
            {
                for (auto c : poly.quad) sk.push_back(c);
                for (auto c : poly.linear) sk.push_back(c);
                sk.push_back(poly.constant);
            }
            return {sk, pk};
        }

        static Bytes sign(const Bytes &sk_bytes, const Bytes &msg)
        {
            qchain::Csprng csprng;
            const size_t qsz = uov_detail::quad_size();
            const size_t per = qsz + P::V + 1;
            if (sk_bytes.size() != P::N * P::N + P::O * per)
                throw std::invalid_argument("UovSignature::sign: bad sk size");

            std::array<std::array<uint8_t, P::N>, P::N> S, S_inv;
            size_t off = 0;
            for (int i = 0; i < P::N; ++i)
                for (int j = 0; j < P::N; ++j)
                    S[i][j] = sk_bytes[off++];
            invert_matrix(S, S_inv);

            CenterMap F;
            for (auto &poly : F.polys)
            {
                poly.quad.assign(sk_bytes.begin() + off, sk_bytes.begin() + off + qsz);
                off += qsz;
                poly.linear.assign(sk_bytes.begin() + off, sk_bytes.begin() + off + P::V);
                off += P::V;
                poly.constant = sk_bytes[off++];
            }

            auto hh = qchain::crypto::sha256(msg);
            std::array<uint8_t, P::O> target;
            for (int i = 0; i < P::O; ++i)
                target[i] = hh[i % hh.size()];

            std::array<uint8_t, P::N> x{};
            for (;;)
            {
                for (int i = 0; i < P::V; ++i)
                    x[P::O + i] = csprng.random_bytes(1)[0];

                std::array<std::array<uint8_t, P::O>, P::O> A{};
                std::array<uint8_t, P::O> b{};
                bool ok = true;
                for (int k = 0; k < P::O; ++k)
                {
                    const auto &poly = F.polys[k];
                    size_t qi = P::V * (P::V + 1) / 2;
                    for (int i = 0; i < P::O; ++i)
                    {
                        uint8_t s = 0;
                        for (int j = 0; j < P::V; ++j)
                            s ^= gf_mul(poly.quad[qi + i * P::V + j], x[P::O + j]);
                        A[k][i] = s;
                    }
                    uint8_t vv = 0;
                    size_t qj = 0;
                    for (int i = 0; i < P::V; ++i)
                        for (int j = i; j < P::V; ++j)
                            vv ^= gf_mul(poly.quad[qj++], gf_mul(x[P::O + i], x[P::O + j]));
                    uint8_t lin = 0;
                    for (int i = 0; i < P::V; ++i)
                        lin ^= gf_mul(poly.linear[i], x[P::O + i]);
                    b[k] = target[k] ^ vv ^ lin ^ poly.constant;
                }

                std::array<uint8_t, P::O> oil{};
                if (!solve_linear(A, b, oil))
                    continue;

                for (int i = 0; i < P::O; ++i)
                    x[i] = oil[i];

                std::array<uint8_t, P::N> sig{};
                for (int i = 0; i < P::N; ++i)
                {
                    uint8_t s = 0;
                    for (int j = 0; j < P::N; ++j)
                        s ^= gf_mul(S_inv[i][j], x[j]);
                    sig[i] = s;
                }
                Bytes out(sig.begin(), sig.end());
                return out;
            }
        }

        static bool verify(const Bytes &pk, const Bytes &msg, const Bytes &sig)
        {
            const size_t per = P::POLY_BYTES;
            if (sig.size() != P::N)
                return false;
            if (pk.size() != P::O * per)
                return false;

            auto hh = qchain::crypto::sha256(msg);
            std::array<uint8_t, P::O> target;
            for (int i = 0; i < P::O; ++i)
                target[i] = hh[i % hh.size()];

            uint8_t x[P::N];
            std::copy(sig.begin(), sig.end(), x);
            for (int k = 0; k < P::O; ++k)
            {
                const uint8_t *c = pk.data() + k * per;
                uint8_t acc = c[per - 1];
                size_t qi = 0;
                for (int a = 0; a < P::N; ++a)
                    for (int b = a; b < P::N; ++b)
                        acc ^= gf_mul(c[qi++], gf_mul(x[a], x[b]));
                for (int m = 0; m < P::N; ++m)
                    acc ^= gf_mul(c[P::QUAD_COUNT + m], x[m]);
                if (acc != target[k])
                    return false;
            }
            return true;
        }

    private:
        static bool invert_matrix(const std::array<std::array<uint8_t, P::N>, P::N> &M,
                                  std::array<std::array<uint8_t, P::N>, P::N> &out)
        {
            std::array<std::array<uint8_t, P::N>, P::N> a = M;
            for (int i = 0; i < P::N; ++i)
            {
                out[i].fill(0);
                out[i][i] = 1;
            }
            for (int col = 0; col < P::N; ++col)
            {
                int piv = -1;
                for (int r = col; r < P::N; ++r)
                    if (a[r][col] != 0) { piv = r; break; }
                if (piv < 0)
                    return false;
                std::swap(a[col], a[piv]);
                std::swap(out[col], out[piv]);
                uint8_t inv = gf_inv(a[col][col]);
                for (int j = 0; j < P::N; ++j) a[col][j] = gf_mul(a[col][j], inv);
                for (int j = 0; j < P::N; ++j) out[col][j] = gf_mul(out[col][j], inv);
                for (int r = 0; r < P::N; ++r)
                {
                    if (r == col) continue;
                    uint8_t f = a[r][col];
                    if (f == 0) continue;
                    for (int j = 0; j < P::N; ++j) a[r][j] ^= gf_mul(f, a[col][j]);
                    for (int j = 0; j < P::N; ++j) out[r][j] ^= gf_mul(f, out[col][j]);
                }
            }
            return true;
        }

        static bool solve_linear(std::array<std::array<uint8_t, P::O>, P::O> A,
                                 std::array<uint8_t, P::O> b,
                                 std::array<uint8_t, P::O> &x)
        {
            for (int col = 0; col < P::O; ++col)
            {
                int piv = -1;
                for (int r = col; r < P::O; ++r)
                    if (A[r][col] != 0) { piv = r; break; }
                if (piv < 0)
                    return false;
                std::swap(A[col], A[piv]);
                std::swap(b[col], b[piv]);
                uint8_t inv = gf_inv(A[col][col]);
                for (int j = col; j < P::O; ++j) A[col][j] = gf_mul(A[col][j], inv);
                b[col] = gf_mul(b[col], inv);
                for (int r = 0; r < P::O; ++r)
                {
                    if (r == col) continue;
                    uint8_t f = A[r][col];
                    if (f == 0) continue;
                    for (int j = col; j < P::O; ++j) A[r][j] ^= gf_mul(f, A[col][j]);
                    b[r] ^= gf_mul(f, b[col]);
                }
            }
            for (int i = 0; i < P::O; ++i)
                x[i] = b[i];
            return true;
        }

        static Bytes build_public_key(const CenterMap &F,
                                      const std::array<std::array<uint8_t, P::N>, P::N> &S)
        {
            const size_t per = P::POLY_BYTES;
            Bytes pk(P::O * per, 0);

            for (int k = 0; k < P::O; ++k)
            {
                uint8_t *c = pk.data() + k * per;
                size_t qi = 0;
                for (int a = 0; a < P::N; ++a)
                    for (int b = a; b < P::N; ++b)
                        c[qi++] = poly_quad_coeff(F.polys[k], S, a, b);
                for (int m = 0; m < P::N; ++m)
                    c[P::QUAD_COUNT + m] = poly_linear_coeff(F.polys[k], S, m);
                c[per - 1] = F.polys[k].constant;
            }
            return pk;
        }

        static uint8_t poly_quad_coeff(const uov_detail::Poly &poly,
                                       const std::array<std::array<uint8_t, P::N>, P::N> &S,
                                       int a, int b)
        {
            uint8_t acc = 0;
            size_t qi = 0;
            for (int i = 0; i < P::V; ++i)
                for (int j = i; j < P::V; ++j)
                {
                    uint8_t c = poly.quad[qi++];
                    if (c == 0) continue;
                    uint8_t t = gf_mul(S[P::O + i][a], S[P::O + j][b]);
                    if (a != b)
                        t ^= gf_mul(S[P::O + i][b], S[P::O + j][a]);
                    acc ^= gf_mul(c, t);
                }
            for (int i = 0; i < P::O; ++i)
                for (int j = 0; j < P::V; ++j)
                {
                    uint8_t c = poly.quad[qi++];
                    if (c == 0) continue;
                    uint8_t t = gf_mul(S[i][a], S[P::O + j][b]);
                    if (a != b)
                        t ^= gf_mul(S[i][b], S[P::O + j][a]);
                    acc ^= gf_mul(c, t);
                }
            return acc;
        }

        static uint8_t poly_linear_coeff(const uov_detail::Poly &poly,
                                         const std::array<std::array<uint8_t, P::N>, P::N> &S,
                                         int a)
        {
            uint8_t acc = 0;
            for (int i = 0; i < P::V; ++i)
                acc ^= gf_mul(poly.linear[i], S[P::O + i][a]);
            return acc;
        }
    };

    }

    using uov_detail::UovSignature;

}