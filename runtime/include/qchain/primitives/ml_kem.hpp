#pragma once
//
// ML-KEM-768 —— 完整 NIST FIPS 203 格密钥封装机制（CCA 安全）
//
// 移植自 pq-crystals/kyber 参考实现（Public Domain / CC0）的 montgomery 域实现：
//   • 模块格 MLWE + 中心二项式采样 + 不完全 NTT + FO 变换（隐式拒绝）。
//   • 系数用带符号 int16（[-q/2, q/2]），NTT 域乘法用 Montgomery 约减。
//
// 参数（ML-KEM-768）：n=256, q=3329, k=3, η₁=η₂=2, du=10, dv=4。
//   公钥 1184 / 私钥 2400 / 密文 1088 / 共享密钥 32 字节。
// 安全性：IND-CCA2，约 192 位经典 / 128 位后量子（NIST 第 3 级）。
//
#include "../common.hpp"
#include "sha3.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace qchain::pqc
{

    using qchain::Byte;
    using qchain::Bytes;

    struct MlKemParams
    {
        static constexpr int N = 256;
        static constexpr int Q = 3329;
        static constexpr int K = 3;      // ML-KEM-768
        static constexpr int ETA1 = 2;
        static constexpr int ETA2 = 2;
        static constexpr int DU = 10;
        static constexpr int DV = 4;
        static constexpr int SYMBYTES = 32;
        static constexpr int POLYBYTES = 384;               // 12-bit × 256
        static constexpr int POLYVECBYTES = K * POLYBYTES;  // 1152
        static constexpr int POLYCOMPRESSEDBYTES = 128;     // 4-bit × 256
        static constexpr int POLYVECCOMPRESSEDBYTES = K * 320; // 10-bit × 256 × 3
        static constexpr int PK_BYTES = 1184;
        static constexpr int SK_BYTES = 2400;
        static constexpr int CT_BYTES = 1088;
        static constexpr int SS_BYTES = 32;
    };

    namespace mlkem_detail
    {
        using P = MlKemParams;

        static constexpr int16_t QINV = -3327; // q^{-1} mod 2^16
        static constexpr int16_t F_INVNTT = 1441; // mont^2/128（invntt 缩放）

        // ─── Montgomery 约减 ──────────────────────────────────────────
        inline int16_t montgomery_reduce(int32_t a)
        {
            int16_t t = static_cast<int16_t>(a) * QINV;
            t = static_cast<int16_t>((a - static_cast<int32_t>(t) * P::Q) >> 16);
            return t;
        }

        // ─── Barrett 约减（中心表示 [-(q-1)/2, (q-1)/2]）──────────────
        inline int16_t barrett_reduce(int16_t a)
        {
            int16_t t;
            const int16_t v = static_cast<int16_t>(((1L << 26) + P::Q / 2) / P::Q);
            t = static_cast<int16_t>(((static_cast<int32_t>(v) * a + (1L << 25)) >> 26));
            t = static_cast<int16_t>(t * P::Q);
            return static_cast<int16_t>(a - t);
        }

        inline int16_t fqmul(int16_t a, int16_t b)
        {
            return montgomery_reduce(static_cast<int32_t>(a) * b);
        }

        // ─── zetas（montgomery 域，128 值）────────────────────────────
        inline const std::array<int16_t, 128> &zetas()
        {
            static const std::array<int16_t, 128> z = {
                -1044, -758, -359, -1517, 1493, 1422, 287, 202,
                -171, 622, 1577, 182, 962, -1202, -1474, 1468,
                573, -1325, 264, 383, -829, 1458, -1602, -130,
                -681, 1017, 732, 608, -1542, 411, -205, -1571,
                1223, 652, -552, 1015, -1293, 1491, -282, -1544,
                516, -8, -320, -666, -1618, -1162, 126, 1469,
                -853, -90, -271, 830, 107, -1421, -247, -951,
                -398, 961, -1508, -725, 448, -1065, 677, -1275,
                -1103, 430, 555, 843, -1251, 871, 1550, 105,
                422, 587, 177, -235, -291, -460, 1574, 1653,
                -246, 778, 1159, -147, -777, 1483, -602, 1119,
                -1590, 644, -872, 349, 418, 329, -156, -75,
                817, 1097, 603, 610, 1322, -1285, -1465, 384,
                -1215, -136, 1218, -1335, -874, 220, -1187, -1659,
                -1185, -1530, -1278, 794, -1510, -854, -870, 478,
                -108, -308, 996, 991, 958, -1460, 1522, 1628};
            return z;
        }

        // ─── NTT（标准序 → 位反序）────────────────────────────────────
        inline void ntt(int16_t r[P::N])
        {
            const auto &z = zetas();
            int k = 1;
            for (int len = 128; len >= 2; len >>= 1)
            {
                for (int start = 0; start < P::N; start = start + 2 * len)
                {
                    int16_t zeta = z[k++];
                    for (int j = start; j < start + len; ++j)
                    {
                        int16_t t = fqmul(zeta, r[j + len]);
                        r[j + len] = static_cast<int16_t>(r[j] - t);
                        r[j] = static_cast<int16_t>(r[j] + t);
                    }
                }
            }
        }

        // ─── invNTT（位反序 → 标准序，输出 montgomery 域）────────────
        inline void invntt(int16_t r[P::N])
        {
            const auto &z = zetas();
            int k = 127;
            for (int len = 2; len <= 128; len <<= 1)
            {
                for (int start = 0; start < P::N; start = start + 2 * len)
                {
                    int16_t zeta = z[k--];
                    for (int j = start; j < start + len; ++j)
                    {
                        int16_t t = r[j];
                        r[j] = barrett_reduce(static_cast<int16_t>(t + r[j + len]));
                        r[j + len] = static_cast<int16_t>(r[j + len] - t);
                        r[j + len] = fqmul(zeta, r[j + len]);
                    }
                }
            }
            for (int j = 0; j < P::N; ++j)
                r[j] = fqmul(r[j], F_INVNTT);
        }

        // ─── 2 元素 basemul（NTT 域，X^2 - zeta）──────────────────────
        inline void basemul(int16_t r[2], const int16_t a[2], const int16_t b[2], int16_t zeta)
        {
            r[0] = fqmul(a[1], b[1]);
            r[0] = fqmul(r[0], zeta);
            r[0] = static_cast<int16_t>(r[0] + fqmul(a[0], b[0]));
            r[1] = fqmul(a[0], b[1]);
            r[1] = static_cast<int16_t>(r[1] + fqmul(a[1], b[0]));
        }

        inline void poly_basemul_montgomery(int16_t r[P::N], const int16_t a[P::N], const int16_t b[P::N])
        {
            const auto &z = zetas();
            for (int i = 0; i < P::N / 4; ++i)
            {
                basemul(&r[4 * i], &a[4 * i], &b[4 * i], z[64 + i]);
                basemul(&r[4 * i + 2], &a[4 * i + 2], &b[4 * i + 2], static_cast<int16_t>(-z[64 + i]));
            }
        }

        // ─── 多项式运算 ───────────────────────────────────────────────
        inline void poly_reduce(int16_t r[P::N])
        {
            for (int i = 0; i < P::N; ++i)
                r[i] = barrett_reduce(r[i]);
        }
        inline void poly_tomont(int16_t r[P::N])
        {
            const int16_t f = static_cast<int16_t>((1ULL << 32) % P::Q);
            for (int i = 0; i < P::N; ++i)
                r[i] = montgomery_reduce(static_cast<int32_t>(r[i]) * f);
        }
        inline void poly_add(int16_t *r, const int16_t *a, const int16_t *b)
        {
            for (int i = 0; i < P::N; ++i)
                r[i] = static_cast<int16_t>(a[i] + b[i]);
        }
        inline void poly_sub(int16_t *r, const int16_t *a, const int16_t *b)
        {
            for (int i = 0; i < P::N; ++i)
                r[i] = static_cast<int16_t>(a[i] - b[i]);
        }

        // ─── CBD 采样（η=2）───────────────────────────────────────────
        inline void cbd2(int16_t *r, const uint8_t *buf)
        {
            for (int i = 0; i < P::N / 8; ++i)
            {
                uint32_t t = static_cast<uint32_t>(buf[4 * i]) |
                             (static_cast<uint32_t>(buf[4 * i + 1]) << 8) |
                             (static_cast<uint32_t>(buf[4 * i + 2]) << 16) |
                             (static_cast<uint32_t>(buf[4 * i + 3]) << 24);
                uint32_t d = t & 0x55555555;
                d += (t >> 1) & 0x55555555;
                for (int j = 0; j < 8; ++j)
                {
                    int a = (d >> (4 * j + 0)) & 0x3;
                    int b = (d >> (4 * j + 2)) & 0x3;
                    r[8 * i + j] = static_cast<int16_t>(a - b);
                }
            }
        }

        // ─── 拒绝采样 uniform（3 字节 → 2 个 12-bit）──────────────────
        inline size_t rej_uniform(int16_t *r, size_t len, const uint8_t *buf, size_t buflen)
        {
            size_t ctr = 0, pos = 0;
            while (ctr < len && pos + 3 <= buflen)
            {
                uint16_t val0 = static_cast<uint16_t>((buf[pos + 0] >> 0) | (static_cast<uint16_t>(buf[pos + 1]) << 8)) & 0xFFF;
                uint16_t val1 = static_cast<uint16_t>((buf[pos + 1] >> 4) | (static_cast<uint16_t>(buf[pos + 2]) << 4)) & 0xFFF;
                pos += 3;
                if (val0 < P::Q)
                    r[ctr++] = static_cast<int16_t>(val0);
                if (ctr < len && val1 < P::Q)
                    r[ctr++] = static_cast<int16_t>(val1);
            }
            return ctr;
        }

        // ─── 压缩/解压 ────────────────────────────────────────────────
        // 多项式（dv=4，128 字节）
        inline void poly_compress(uint8_t *r, const int16_t *a)
        {
            uint8_t t[8];
            for (int i = 0; i < P::N / 8; ++i)
            {
                for (int j = 0; j < 8; ++j)
                {
                    int16_t u = a[8 * i + j];
                    u = static_cast<int16_t>(u + ((u >> 15) & P::Q));
                    uint32_t d0 = static_cast<uint32_t>(u) << 4;
                    d0 += 1665;
                    d0 *= 80635;
                    d0 >>= 28;
                    t[j] = static_cast<uint8_t>(d0 & 0xf);
                }
                r[0] = static_cast<uint8_t>(t[0] | (t[1] << 4));
                r[1] = static_cast<uint8_t>(t[2] | (t[3] << 4));
                r[2] = static_cast<uint8_t>(t[4] | (t[5] << 4));
                r[3] = static_cast<uint8_t>(t[6] | (t[7] << 4));
                r += 4;
            }
        }
        inline void poly_decompress(int16_t *r, const uint8_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                r[2 * i + 0] = static_cast<int16_t>(((static_cast<uint16_t>(a[0] & 15) * P::Q) + 8) >> 4);
                r[2 * i + 1] = static_cast<int16_t>(((static_cast<uint16_t>(a[0] >> 4) * P::Q) + 8) >> 4);
                a += 1;
            }
        }
        // 向量（du=10，每多项式 320 字节）
        inline void polyvec_compress(uint8_t *r, const int16_t a[P::K][P::N])
        {
            uint16_t t[4];
            for (int i = 0; i < P::K; ++i)
            {
                for (int j = 0; j < P::N / 4; ++j)
                {
                    for (int k = 0; k < 4; ++k)
                    {
                        t[k] = static_cast<uint16_t>(a[i][4 * j + k]);
                        t[k] = static_cast<uint16_t>(t[k] + ((static_cast<int16_t>(t[k]) >> 15) & P::Q));
                        uint64_t d0 = t[k];
                        d0 <<= 10;
                        d0 += 1665;
                        d0 *= 1290167;
                        d0 >>= 32;
                        t[k] = static_cast<uint16_t>(d0 & 0x3ff);
                    }
                    r[0] = static_cast<uint8_t>(t[0] >> 0);
                    r[1] = static_cast<uint8_t>((t[0] >> 8) | (t[1] << 2));
                    r[2] = static_cast<uint8_t>((t[1] >> 6) | (t[2] << 4));
                    r[3] = static_cast<uint8_t>((t[2] >> 4) | (t[3] << 6));
                    r[4] = static_cast<uint8_t>(t[3] >> 2);
                    r += 5;
                }
            }
        }
        inline void polyvec_decompress(int16_t a[P::K][P::N], const uint8_t *r)
        {
            uint16_t t[4];
            for (int i = 0; i < P::K; ++i)
            {
                for (int j = 0; j < P::N / 4; ++j)
                {
                    t[0] = static_cast<uint16_t>((r[0] >> 0) | (static_cast<uint16_t>(r[1]) << 8));
                    t[1] = static_cast<uint16_t>((r[1] >> 2) | (static_cast<uint16_t>(r[2]) << 6));
                    t[2] = static_cast<uint16_t>((r[2] >> 4) | (static_cast<uint16_t>(r[3]) << 4));
                    t[3] = static_cast<uint16_t>((r[3] >> 6) | (static_cast<uint16_t>(r[4]) << 2));
                    r += 5;
                    for (int k = 0; k < 4; ++k)
                        a[i][4 * j + k] = static_cast<int16_t>((static_cast<uint32_t>(t[k] & 0x3FF) * P::Q + 512) >> 10);
                }
            }
        }

        // ─── 12-bit 序列化 ────────────────────────────────────────────
        inline void poly_tobytes(uint8_t *r, const int16_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                uint16_t t0 = static_cast<uint16_t>(a[2 * i]);
                t0 = static_cast<uint16_t>(t0 + ((static_cast<int16_t>(t0) >> 15) & P::Q));
                uint16_t t1 = static_cast<uint16_t>(a[2 * i + 1]);
                t1 = static_cast<uint16_t>(t1 + ((static_cast<int16_t>(t1) >> 15) & P::Q));
                r[3 * i + 0] = static_cast<uint8_t>(t0 >> 0);
                r[3 * i + 1] = static_cast<uint8_t>((t0 >> 8) | (t1 << 4));
                r[3 * i + 2] = static_cast<uint8_t>(t1 >> 4);
            }
        }
        inline void poly_frombytes(int16_t *r, const uint8_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                r[2 * i] = static_cast<int16_t>((a[3 * i + 0] >> 0) | (static_cast<uint16_t>(a[3 * i + 1]) << 8)) & 0xFFF;
                r[2 * i + 1] = static_cast<int16_t>((a[3 * i + 1] >> 4) | (static_cast<uint16_t>(a[3 * i + 2]) << 4)) & 0xFFF;
            }
        }

        // ─── 消息 ↔ 多项式 ────────────────────────────────────────────
        inline void poly_frommsg(int16_t *r, const uint8_t msg[P::SS_BYTES])
        {
            for (int i = 0; i < P::N / 8; ++i)
                for (int j = 0; j < 8; ++j)
                    r[8 * i + j] = ((msg[i] >> j) & 1) ? static_cast<int16_t>((P::Q + 1) / 2) : 0;
        }
        inline void poly_tomsg(uint8_t msg[P::SS_BYTES], const int16_t *a)
        {
            for (int i = 0; i < P::N / 8; ++i)
            {
                msg[i] = 0;
                for (int j = 0; j < 8; ++j)
                {
                    // 带符号 int16 → uint32（符号扩展），定点 round(2/q · x)
                    uint32_t t = static_cast<uint32_t>(a[8 * i + j]);
                    t <<= 1;
                    t += 1665;
                    t *= 80635;
                    t >>= 28;
                    t &= 1;
                    msg[i] = static_cast<uint8_t>(msg[i] | (t << j));
                }
            }
        }

        // ─── 向量 NTT ─────────────────────────────────────────────────
        inline void polyvec_ntt(int16_t v[P::K][P::N])
        {
            for (int i = 0; i < P::K; ++i)
            {
                ntt(v[i]);
                poly_reduce(v[i]);
            }
        }
        inline void polyvec_invntt_tomont(int16_t v[P::K][P::N])
        {
            for (int i = 0; i < P::K; ++i)
                invntt(v[i]);
        }
        inline void polyvec_basemul_acc_montgomery(int16_t r[P::N], const int16_t a[P::K][P::N], const int16_t b[P::K][P::N])
        {
            poly_basemul_montgomery(r, a[0], b[0]);
            for (int i = 1; i < P::K; ++i)
            {
                int16_t t[P::N];
                poly_basemul_montgomery(t, a[i], b[i]);
                poly_add(r, r, t);
            }
            poly_reduce(r);
        }
        inline void polyvec_add(int16_t r[P::K][P::N], const int16_t a[P::K][P::N], const int16_t b[P::K][P::N])
        {
            for (int i = 0; i < P::K; ++i)
                poly_add(r[i], a[i], b[i]);
        }
        inline void polyvec_reduce(int16_t v[P::K][P::N])
        {
            for (int i = 0; i < P::K; ++i)
                poly_reduce(v[i]);
        }

        // ─── gen_matrix（A 从 seed 采样，transposed 控制 A/A^T）──────
        inline void gen_matrix(int16_t a[P::K][P::K][P::N], const uint8_t seed[P::SYMBYTES], int transposed)
        {
            for (int i = 0; i < P::K; ++i)
            {
                for (int j = 0; j < P::K; ++j)
                {
                    Bytes in(seed, seed + P::SYMBYTES);
                    in.push_back(static_cast<Byte>(transposed ? i : j));
                    in.push_back(static_cast<Byte>(transposed ? j : i));
                    // 一次生成足够字节（1536 字节 → 512 个 12-bit → rejection 后 > 256 系数）。
                    Bytes xof = qchain::crypto::shake128(in, 3 * P::N * 2);
                    size_t ctr = rej_uniform(a[i][j], P::N, xof.data(), xof.size());
                    if (ctr < P::N)
                        throw std::runtime_error("ML-KEM gen_matrix: insufficient XOF output");
                }
            }
        }

        // ─── K-PKE（CPA 核心）────────────────────────────────────────
        inline void indcpa_keypair(uint8_t pk[P::PK_BYTES], uint8_t sk[P::POLYVECBYTES], const uint8_t coins[P::SYMBYTES])
        {
            uint8_t buf[2 * P::SYMBYTES];
            std::memcpy(buf, coins, P::SYMBYTES);
            buf[P::SYMBYTES] = static_cast<uint8_t>(P::K);
            Bytes g = qchain::crypto::sha3_512(Bytes(buf, buf + P::SYMBYTES + 1));
            const uint8_t *publicseed = g.data();
            const uint8_t *noiseseed = g.data() + P::SYMBYTES;

            int16_t a[P::K][P::K][P::N], e[P::K][P::N], skpv[P::K][P::N], pkpv[P::K][P::N];
            gen_matrix(a, publicseed, 0);

            uint8_t nonce = 0;
            for (int i = 0; i < P::K; ++i)
            {
                Bytes prf(noiseseed, noiseseed + P::SYMBYTES);
                prf.push_back(nonce++);
                Bytes out = qchain::crypto::shake256(prf, 64 * P::ETA1);
                cbd2(skpv[i], out.data());
            }
            for (int i = 0; i < P::K; ++i)
            {
                Bytes prf(noiseseed, noiseseed + P::SYMBYTES);
                prf.push_back(nonce++);
                Bytes out = qchain::crypto::shake256(prf, 64 * P::ETA1);
                cbd2(e[i], out.data());
            }

            polyvec_ntt(skpv);
            polyvec_ntt(e);

            for (int i = 0; i < P::K; ++i)
            {
                polyvec_basemul_acc_montgomery(pkpv[i], a[i], skpv);
                poly_tomont(pkpv[i]);
            }
            polyvec_add(pkpv, pkpv, e);
            polyvec_reduce(pkpv);

            for (int i = 0; i < P::K; ++i)
                poly_tobytes(sk + i * P::POLYBYTES, skpv[i]);
            for (int i = 0; i < P::K; ++i)
                poly_tobytes(pk + i * P::POLYBYTES, pkpv[i]);
            std::memcpy(pk + P::POLYVECBYTES, publicseed, P::SYMBYTES);
        }

        inline void indcpa_enc(uint8_t c[P::CT_BYTES], const uint8_t m[P::SS_BYTES],
                               const uint8_t pk[P::PK_BYTES], const uint8_t coins[P::SYMBYTES])
        {
            uint8_t seed[P::SYMBYTES];
            int16_t sp[P::K][P::N], pkpv[P::K][P::N], ep[P::K][P::N], at[P::K][P::K][P::N], b[P::K][P::N];
            int16_t v[P::N], k[P::N], epp[P::N];

            for (int i = 0; i < P::K; ++i)
                poly_frombytes(pkpv[i], pk + i * P::POLYBYTES);
            std::memcpy(seed, pk + P::POLYVECBYTES, P::SYMBYTES);
            poly_frommsg(k, m);
            gen_matrix(at, seed, 1);

            uint8_t nonce = 0;
            for (int i = 0; i < P::K; ++i)
            {
                Bytes prf(coins, coins + P::SYMBYTES);
                prf.push_back(nonce++);
                Bytes out = qchain::crypto::shake256(prf, 64 * P::ETA1);
                cbd2(sp[i], out.data());
            }
            for (int i = 0; i < P::K; ++i)
            {
                Bytes prf(coins, coins + P::SYMBYTES);
                prf.push_back(nonce++);
                Bytes out = qchain::crypto::shake256(prf, 64 * P::ETA2);
                cbd2(ep[i], out.data());
            }
            {
                Bytes prf(coins, coins + P::SYMBYTES);
                prf.push_back(nonce++);
                Bytes out = qchain::crypto::shake256(prf, 64 * P::ETA2);
                cbd2(epp, out.data());
            }

            polyvec_ntt(sp);

            for (int i = 0; i < P::K; ++i)
                polyvec_basemul_acc_montgomery(b[i], at[i], sp);
            polyvec_basemul_acc_montgomery(v, pkpv, sp);

            polyvec_invntt_tomont(b);
            invntt(v);

            polyvec_add(b, b, ep);
            poly_add(v, v, epp);
            poly_add(v, v, k);
            polyvec_reduce(b);
            poly_reduce(v);

            polyvec_compress(c, b);
            poly_compress(c + P::POLYVECCOMPRESSEDBYTES, v);
        }

        inline void indcpa_dec(uint8_t m[P::SS_BYTES], const uint8_t c[P::CT_BYTES], const uint8_t sk[P::POLYVECBYTES])
        {
            int16_t b[P::K][P::N], skpv[P::K][P::N], mp[P::N], v[P::N];

            polyvec_decompress(b, c);
            poly_decompress(v, c + P::POLYVECCOMPRESSEDBYTES);
            for (int i = 0; i < P::K; ++i)
                poly_frombytes(skpv[i], sk + i * P::POLYBYTES);

            polyvec_ntt(b);
            polyvec_basemul_acc_montgomery(mp, skpv, b);
            invntt(mp);

            poly_sub(mp, v, mp);
            poly_reduce(mp);

            poly_tomsg(m, mp);
        }

    }
    
    // ══════════════════════════════════════════════════════════════════
    // ML-KEM-768（FO 变换，IND-CCA2）
    // ══════════════════════════════════════════════════════════════════
    class MlKem768
    {
        using P = MlKemParams;

    public:
        // ── 确定性密钥生成（coins = d || z，共 64 字节，可复现/审计/KAT）──
        static std::pair<Bytes, Bytes> keygen_derand(const Bytes &coins)
        {
            if (coins.size() != 2 * P::SYMBYTES)
                throw std::invalid_argument("MlKem768::keygen_derand: coins must be 64 bytes");

            Bytes pk(P::PK_BYTES);
            Bytes sk(P::SK_BYTES);
            mlkem_detail::indcpa_keypair(pk.data(), sk.data(), coins.data());

            // sk = sk_pke || pk || H(pk) || z
            std::copy(pk.begin(), pk.end(), sk.begin() + P::POLYVECBYTES);
            Bytes h = qchain::crypto::sha3_256(pk);
            std::copy(h.begin(), h.end(), sk.begin() + P::POLYVECBYTES + P::PK_BYTES);
            std::copy(coins.begin() + P::SYMBYTES, coins.end(), sk.begin() + P::POLYVECBYTES + P::PK_BYTES + P::SYMBYTES);
            return {pk, sk};
        }

        static std::pair<Bytes, Bytes> keygen()
        {
            qchain::Csprng csprng;
            return keygen_derand(csprng.random_bytes(2 * P::SYMBYTES));
        }

        // ── 确定性封装（coins = m，32 字节随机消息）────────────────────
        static std::pair<Bytes, Bytes> encaps_derand(const Bytes &pk, const Bytes &coins)
        {
            if (pk.size() != P::PK_BYTES)
                throw std::invalid_argument("MlKem768::encaps_derand: bad pk size");
            if (coins.size() != P::SYMBYTES)
                throw std::invalid_argument("MlKem768::encaps_derand: coins must be 32 bytes");

            Bytes m = coins;
            Bytes buf = m;
            Bytes h = qchain::crypto::sha3_256(pk);
            buf.insert(buf.end(), h.begin(), h.end());
            Bytes kr = qchain::crypto::sha3_512(buf);
            Bytes K(kr.begin(), kr.begin() + P::SYMBYTES);
            Bytes r(kr.begin() + P::SYMBYTES, kr.end());

            Bytes ct(P::CT_BYTES);
            mlkem_detail::indcpa_enc(ct.data(), m.data(), pk.data(), r.data());
            return {ct, K};
        }

        static std::pair<Bytes, Bytes> encaps(const Bytes &pk)
        {
            qchain::Csprng csprng;
            return encaps_derand(pk, csprng.random_bytes(P::SYMBYTES));
        }

        static Bytes decaps(const Bytes &sk, const Bytes &ct)
        {
            if (sk.size() != P::SK_BYTES || ct.size() != P::CT_BYTES)
                throw std::invalid_argument("MlKem768::decaps: bad size");

            const uint8_t *sk_pke = sk.data();
            const uint8_t *pk = sk.data() + P::POLYVECBYTES;
            const uint8_t *h = sk.data() + P::POLYVECBYTES + P::PK_BYTES;
            const uint8_t *z = sk.data() + P::POLYVECBYTES + P::PK_BYTES + P::SYMBYTES;

            Bytes mp(P::SS_BYTES);
            mlkem_detail::indcpa_dec(mp.data(), ct.data(), sk_pke);

            Bytes buf = mp;
            buf.insert(buf.end(), h, h + P::SYMBYTES);
            Bytes kr = qchain::crypto::sha3_512(buf);
            Bytes Kp(kr.begin(), kr.begin() + P::SYMBYTES);
            Bytes rp(kr.begin() + P::SYMBYTES, kr.end());

            Bytes ctp(P::CT_BYTES);
            mlkem_detail::indcpa_enc(ctp.data(), mp.data(), pk, rp.data());

            bool fail = !qchain::constant_time_equal(ct, ctp);

            // 隐式拒绝：K = K' 或 SHAKE256(z || ct, 32)
            Bytes zc(z, z + P::SYMBYTES);
            zc.insert(zc.end(), ct.begin(), ct.end());
            Bytes j = qchain::crypto::shake256(zc, P::SS_BYTES);

            Bytes K(P::SS_BYTES);
            for (size_t i = 0; i < K.size(); ++i)
                K[i] = static_cast<Byte>(fail ? j[i] : Kp[i]);

            qchain::secure_wipe(mp);
            qchain::secure_wipe(Kp);
            qchain::secure_wipe(rp);
            return K;
        }
    };

}