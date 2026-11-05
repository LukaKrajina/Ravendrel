#pragma once
//
// UOV —— 非平衡油醋多变量签名（基于 MQ 问题）
//
// 与格（ML-KEM/ML-DSA）、哈希（Lamport/Merkle）不同的第三大后量子范式：
// 安全性基于「有限域上求解多元二次方程组（MQ 问题）」的 NP-hard 困难性，
// 由 Patarin 1997 / Kipnis-Shamir 1999 提出，经 27 年密码分析验证，是最成熟的多变量方案。
//
// 结构（Unbalanced Oil and Vinegar）：
//   • 变量分 o 个「油」+ v 个「醋」（n = o+v）；油变量之间不相乘。
//   • 私钥 = 中心映射 F（油×醋 + 醋×醋二次型）+ 可逆线性变换 S。
//   • 公钥 = P = F∘S（隐藏中心结构，随机化后看似一般 MQ）。
//   • 签名：固定随机醋变量 → 油变量退化为线性方程组 → 高斯消元求逆。
//
// 本实现为 GF(256) 上的 UOV（参数见 MlDsaParams 同级的 UovParams），
// 签名极小（= o 字节，远小于 Dilithium），公钥较大（多变量范式的固有权衡）。
// 参考：NIST 第四轮 UOV 规范（pqov/pqov）。
//
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
        static constexpr int O = 32; // 油变量数（= 签名字节数）
        static constexpr int V = 48; // 醋变量数
        static constexpr int N = O + V; // 80
        // 公钥 = P = F∘S 是「一般二次型」：N(N+1)/2 个二次项（上三角）+ N 个线性项 + 1 常数。
        static constexpr int QUAD_COUNT = N * (N + 1) / 2;
        static constexpr int POLY_BYTES = QUAD_COUNT + N + 1;
        static constexpr int PK_BYTES = O * POLY_BYTES;
        static constexpr int SK_BYTES = N * N + O * (1 + V + O * V + V * (V + 1) / 2); // S + F（UOV 结构）
    };

    namespace uov_detail
    {
        using P = UovParams;

        // ─── GF(256)（不可约多项式 x^8+x^4+x^3+x+1，生成元 3）────────
        inline const std::array<uint8_t, 256> &gf_exp()
        {
            static const std::array<uint8_t, 256> exp = []()
            {
                std::array<uint8_t, 256> e{};
                int x = 1;
                for (int i = 0; i < 255; ++i)
                {
                    e[i] = static_cast<uint8_t>(x);
                    // x = x * 3（生成元 3，阶 255）
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

        // 中心映射多项式求值：f = Σ 二次项 + Σ 线性项 + 常数。
        // F[k] 存储系数布局：二次项（油×醋 + 醋×醋）+ 线性 + 常数。
        struct Poly
        {
            // 系数：二次项 V*(V+1)/2 个（醋×醋）+ O*V 个（油×醋）+ V 线性 + 1 常数
            std::vector<uint8_t> quad;   // O*V + V*(V+1)/2
            std::vector<uint8_t> linear; // V
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

    // ══════════════════════════════════════════════════════════════════
    // UovSignature —— 多变量签名（GF(256)），定义于 uov_detail 内。
    // ══════════════════════════════════════════════════════════════════
    class UovSignature
    {
        using P = UovParams;

        struct CenterMap
        {
            std::array<uov_detail::Poly, P::O> polys;
        };

    public:
        // ── 密钥生成 ─────────────────────────────────────────────────
        static std::pair<Bytes, Bytes> keygen()
        {
            qchain::Csprng csprng;

            // 1. 随机中心映射 F（油×醋 + 醋×醋 二次型）
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

            // 2. 随机可逆线性变换 S（n×n，GF(256) 上可逆）
            std::array<std::array<uint8_t, P::N>, P::N> S, S_inv;
            for (int i = 0; i < P::N; ++i)
                for (int j = 0; j < P::N; ++j)
                    S[i][j] = csprng.random_bytes(1)[0];
            if (!invert_matrix(S, S_inv))
                return keygen(); // 退化重试

            // 3. 公钥 P = F∘S：把 S 代入 F 的多项式，展开得到一般二次型系数。
            Bytes pk = build_public_key(F, S);

            // 4. 私钥 = S + F（序列化）
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

        // ── 签名 ─────────────────────────────────────────────────────
        static Bytes sign(const Bytes &sk_bytes, const Bytes &msg)
        {
            qchain::Csprng csprng;
            // 解析私钥
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

            // 目标 h = SHA-256(msg) 映射到 o 个 GF(256) 元素
            auto hh = qchain::crypto::sha256(msg);
            std::array<uint8_t, P::O> target;
            for (int i = 0; i < P::O; ++i)
                target[i] = hh[i % hh.size()];

            // 尝试签名（醋变量随机，解线性方程组）
            std::array<uint8_t, P::N> x{};
            for (;;)
            {
                // 随机醋变量
                for (int i = 0; i < P::V; ++i)
                    x[P::O + i] = csprng.random_bytes(1)[0];

                // 代入醋变量，得到油变量的线性方程组：A·oil = b
                // f_k = Σ_i oil_i · (Σ_j a_{ij}^{(k)} vine_j) + (醋项)
                std::array<std::array<uint8_t, P::O>, P::O> A{};
                std::array<uint8_t, P::O> b{};
                bool ok = true;
                for (int k = 0; k < P::O; ++k)
                {
                    const auto &poly = F.polys[k];
                    // 油×醋 系数：quad 的布局是 [醋×醋 | 油×醋]
                    size_t qi = P::V * (P::V + 1) / 2; // 跳过醋×醋
                    // 油×醋：A[k][i] = Σ_j c_{ij}^{(k)} · vine_j
                    for (int i = 0; i < P::O; ++i)
                    {
                        uint8_t s = 0;
                        for (int j = 0; j < P::V; ++j)
                            s ^= gf_mul(poly.quad[qi + i * P::V + j], x[P::O + j]);
                        A[k][i] = s;
                    }
                    // b[k] = target[k] ⊕ (醋×醋 + 醋线性 + 常数)
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

                // 高斯消元解 A·oil = b
                std::array<uint8_t, P::O> oil{};
                if (!solve_linear(A, b, oil))
                    continue; // 奇异，重试

                for (int i = 0; i < P::O; ++i)
                    x[i] = oil[i];

                // S^{-1} 变换得到最终签名
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

        // ── 验证 ─────────────────────────────────────────────────────
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

            // 对签名 x，计算 P_k(x)（一般二次型）
            uint8_t x[P::N];
            std::copy(sig.begin(), sig.end(), x);
            for (int k = 0; k < P::O; ++k)
            {
                const uint8_t *c = pk.data() + k * per;
                uint8_t acc = c[per - 1]; // 常数
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
        // GF(256) 高斯消元（含求逆），返回是否可逆。
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

        // 解线性方程组 A·x = b（A 是 O×O），返回是否可解。
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

        // 展开公钥 P = F∘S（一般二次型）。
        static Bytes build_public_key(const CenterMap &F,
                                      const std::array<std::array<uint8_t, P::N>, P::N> &S)
        {
            const size_t per = P::POLY_BYTES;
            Bytes pk(P::O * per, 0);

            for (int k = 0; k < P::O; ++k)
            {
                uint8_t *c = pk.data() + k * per;
                size_t qi = 0;
                // 二次项：所有变量对 (a, b)，a ≤ b（上三角，含油×油/油×醋/醋×醋）
                for (int a = 0; a < P::N; ++a)
                    for (int b = a; b < P::N; ++b)
                        c[qi++] = poly_quad_coeff(F.polys[k], S, a, b);
                // 线性项：所有变量
                for (int m = 0; m < P::N; ++m)
                    c[P::QUAD_COUNT + m] = poly_linear_coeff(F.polys[k], S, m);
                // 常数项
                c[per - 1] = F.polys[k].constant;
            }
            return pk;
        }

        // 计算中心映射多项式 f_k 经过线性变换 S 后，单项式 x_a·x_b 的系数。
        static uint8_t poly_quad_coeff(const uov_detail::Poly &poly,
                                       const std::array<std::array<uint8_t, P::N>, P::N> &S,
                                       int a, int b)
        {
            uint8_t acc = 0;
            // f_k 的油×醋 + 醋×醋 项
            size_t qi = 0;
            // 醋×醋
            for (int i = 0; i < P::V; ++i)
                for (int j = i; j < P::V; ++j)
                {
                    uint8_t c = poly.quad[qi++];
                    if (c == 0) continue;
                    // 贡献 = c · (S[O+i][a]·S[O+j][b] + S[O+i][b]·S[O+j][a])
                    uint8_t t = gf_mul(S[P::O + i][a], S[P::O + j][b]);
                    if (a != b)
                        t ^= gf_mul(S[P::O + i][b], S[P::O + j][a]);
                    acc ^= gf_mul(c, t);
                }
            // 油×醋
            for (int i = 0; i < P::O; ++i)
                for (int j = 0; j < P::V; ++j)
                {
                    uint8_t c = poly.quad[qi++];
                    if (c == 0) continue;
                    // 贡献 = c · (S[i][a]·S[O+j][b] + S[i][b]·S[O+j][a])
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

    // 导出 UovSignature 到 qchain::pqc 命名空间。
    using uov_detail::UovSignature;

}