#pragma once
//
// ML-DSA-65（Dilithium）—— 完整 NIST FIPS 204 格数字签名（EUF-CMA 安全）
//
// 移植自 pq-crystals/dilithium 参考实现（Public Domain / CC0）。
// 模块格 MLWE + Fiat-Shamir with aborts（拒绝采样）+ 高位/低位分解 + 提示。
//
// 参数（ML-DSA-65，DILITHIUM_MODE=3）：q=8380417, n=256, k=6, l=5, η=4, τ=49。
//   公钥 1952 / 私钥 4032 / 签名 3309 字节。NIST 安全等级 3（约 192 位经典 / 128 位后量子）。
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

    struct MlDsaParams
    {
        static constexpr int N = 256;
        static constexpr int64_t Q = 8380417;
        static constexpr int K = 6;   // ML-DSA-65
        static constexpr int L = 5;
        static constexpr int ETA = 4;
        static constexpr int D = 13;
        static constexpr int TAU = 49;
        static constexpr int BETA = 196;
        static constexpr int64_t GAMMA1 = 1 << 19;
        static constexpr int64_t GAMMA2 = (Q - 1) / 32;
        static constexpr int OMEGA = 55;
        static constexpr int SEEDBYTES = 32;
        static constexpr int CRHBYTES = 64;
        static constexpr int TRBYTES = 64;
        static constexpr int CTILDEBYTES = 48;
        static constexpr int POLYT1_PACKEDBYTES = 320;
        static constexpr int POLYT0_PACKEDBYTES = 416;
        static constexpr int POLYETA_PACKEDBYTES = 128;
        static constexpr int POLYZ_PACKEDBYTES = 640;
        static constexpr int POLYW1_PACKEDBYTES = 128;
        static constexpr int PK_BYTES = 1952;
        static constexpr int SK_BYTES = 4032;
        static constexpr int SIG_BYTES = 3309;
    };

    namespace mldsa_detail
    {
        using P = MlDsaParams;

        static constexpr int32_t QINV = 58728449; // q^{-1} mod 2^32
        static constexpr int32_t F_INVNTT = 41978; // mont^2/256

        inline int32_t montgomery_reduce(int64_t a)
        {
            int32_t t = static_cast<int32_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * QINV);
            t = static_cast<int32_t>((a - static_cast<int64_t>(t) * P::Q) >> 32);
            return t;
        }
        inline int32_t reduce32(int32_t a)
        {
            int32_t t = (a + (1 << 22)) >> 23;
            t = a - t * static_cast<int32_t>(P::Q);
            return t;
        }
        inline int32_t caddq(int32_t a)
        {
            a += (a >> 31) & static_cast<int32_t>(P::Q);
            return a;
        }
        inline int32_t freeze(int32_t a) { return caddq(reduce32(a)); }

        inline const std::array<int32_t, 256> &zetas()
        {
            static const std::array<int32_t, 256> z = {
                0, 25847, -2608894, -518909, 237124, -777960, -876248, 466468,
                1826347, 2353451, -359251, -2091905, 3119733, -2884855, 3111497, 2680103,
                2725464, 1024112, -1079900, 3585928, -549488, -1119584, 2619752, -2108549,
                -2118186, -3859737, -1399561, -3277672, 1757237, -19422, 4010497, 280005,
                2706023, 95776, 3077325, 3530437, -1661693, -3592148, -2537516, 3915439,
                -3861115, -3043716, 3574422, -2867647, 3539968, -300467, 2348700, -539299,
                -1699267, -1643818, 3505694, -3821735, 3507263, -2140649, -1600420, 3699596,
                811944, 531354, 954230, 3881043, 3900724, -2556880, 2071892, -2797779,
                -3930395, -1528703, -3677745, -3041255, -1452451, 3475950, 2176455, -1585221,
                -1257611, 1939314, -4083598, -1000202, -3190144, -3157330, -3632928, 126922,
                3412210, -983419, 2147896, 2715295, -2967645, -3693493, -411027, -2477047,
                -671102, -1228525, -22981, -1308169, -381987, 1349076, 1852771, -1430430,
                -3343383, 264944, 508951, 3097992, 44288, -1100098, 904516, 3958618,
                -3724342, -8578, 1653064, -3249728, 2389356, -210977, 759969, -1316856,
                189548, -3553272, 3159746, -1851402, -2409325, -177440, 1315589, 1341330,
                1285669, -1584928, -812732, -1439742, -3019102, -3881060, -3628969, 3839961,
                2091667, 3407706, 2316500, 3817976, -3342478, 2244091, -2446433, -3562462,
                266997, 2434439, -1235728, 3513181, -3520352, -3759364, -1197226, -3193378,
                900702, 1859098, 909542, 819034, 495491, -1613174, -43260, -522500,
                -655327, -3122442, 2031748, 3207046, -3556995, -525098, -768622, -3595838,
                342297, 286988, -2437823, 4108315, 3437287, -3342277, 1735879, 203044,
                2842341, 2691481, -2590150, 1265009, 4055324, 1247620, 2486353, 1595974,
                -3767016, 1250494, 2635921, -3548272, -2994039, 1869119, 1903435, -1050970,
                -1333058, 1237275, -3318210, -1430225, -451100, 1312455, 3306115, -1962642,
                -1279661, 1917081, -2546312, -1374803, 1500165, 777191, 2235880, 3406031,
                -542412, -2831860, -1671176, -1846953, -2584293, -3724270, 594136, -3776993,
                -2013608, 2432395, 2454455, -164721, 1957272, 3369112, 185531, -1207385,
                -3183426, 162844, 1616392, 3014001, 810149, 1652634, -3694233, -1799107,
                -3038916, 3523897, 3866901, 269760, 2213111, -975884, 1717735, 472078,
                -426683, 1723600, -1803090, 1910376, -1667432, -1104333, -260646, -3833893,
                -2939036, -2235985, -420899, -2286327, 183443, -976891, 1612842, -3545687,
                -554416, 3919660, -48306, -1362209, 3937738, 1400424, -846154, 1976782};
            return z;
        }

        inline void ntt(int32_t a[P::N])
        {
            const auto &z = zetas();
            int k = 0;
            for (int len = 128; len > 0; len >>= 1)
                for (int start = 0; start < P::N; start += 2 * len)
                {
                    int32_t zeta = z[++k];
                    for (int j = start; j < start + len; ++j)
                    {
                        int32_t t = montgomery_reduce(static_cast<int64_t>(zeta) * a[j + len]);
                        a[j + len] = a[j] - t;
                        a[j] = a[j] + t;
                    }
                }
        }

        inline void invntt_tomont(int32_t a[P::N])
        {
            const auto &z = zetas();
            int k = 256;
            for (int len = 1; len < P::N; len <<= 1)
                for (int start = 0; start < P::N; start += 2 * len)
                {
                    int32_t zeta = -z[--k];
                    for (int j = start; j < start + len; ++j)
                    {
                        int32_t t = a[j];
                        a[j] = t + a[j + len];
                        a[j + len] = t - a[j + len];
                        a[j + len] = montgomery_reduce(static_cast<int64_t>(zeta) * a[j + len]);
                    }
                }
            for (int j = 0; j < P::N; ++j)
                a[j] = montgomery_reduce(static_cast<int64_t>(F_INVNTT) * a[j]);
        }

        // ─── 舍入 ─────────────────────────────────────────────────
        inline int32_t power2round(int32_t *a0, int32_t a)
        {
            int32_t a1 = (a + (1 << (P::D - 1)) - 1) >> P::D;
            *a0 = a - (a1 << P::D);
            return a1;
        }
        inline int32_t decompose(int32_t *a0, int32_t a)
        {
            int32_t a1 = (a + 127) >> 7;
            a1 = (a1 * 1025 + (1 << 21)) >> 22;
            a1 &= 15;
            *a0 = a - a1 * 2 * static_cast<int32_t>(P::GAMMA2);
            *a0 -= (((static_cast<int32_t>(P::Q) - 1) / 2 - *a0) >> 31) & static_cast<int32_t>(P::Q);
            return a1;
        }
        inline unsigned int make_hint(int32_t a0, int32_t a1)
        {
            if (a0 > P::GAMMA2 || a0 < -P::GAMMA2 || (a0 == -P::GAMMA2 && a1 != 0))
                return 1;
            return 0;
        }
        inline int32_t use_hint(int32_t a, unsigned int hint)
        {
            int32_t a0, a1 = decompose(&a0, a);
            if (hint == 0)
                return a1;
            if (a0 > 0)
                return (a1 + 1) & 15;
            return (a1 - 1) & 15;
        }

        // ─── 采样 ─────────────────────────────────────────────────────
        inline Bytes shake128_stream(const uint8_t *seed, uint16_t nonce, size_t out_len)
        {
            Bytes in(seed, seed + P::SEEDBYTES);
            in.push_back(static_cast<Byte>(nonce & 0xFF));
            in.push_back(static_cast<Byte>(nonce >> 8));
            return qchain::crypto::shake128(in, out_len);
        }
        inline Bytes shake256_stream(const uint8_t *seed, uint16_t nonce, size_t out_len)
        {
            Bytes in(seed, seed + P::CRHBYTES);
            in.push_back(static_cast<Byte>(nonce & 0xFF));
            in.push_back(static_cast<Byte>(nonce >> 8));
            return qchain::crypto::shake256(in, out_len);
        }

        inline size_t rej_uniform(int32_t *a, size_t len, const uint8_t *buf, size_t buflen)
        {
            size_t ctr = 0, pos = 0;
            while (ctr < len && pos + 3 <= buflen)
            {
                uint32_t t = static_cast<uint32_t>(buf[pos]) |
                             (static_cast<uint32_t>(buf[pos + 1]) << 8) |
                             (static_cast<uint32_t>(buf[pos + 2]) << 16);
                pos += 3;
                t &= 0x7FFFFF;
                if (t < P::Q)
                    a[ctr++] = static_cast<int32_t>(t);
            }
            return ctr;
        }

        inline void poly_uniform(int32_t *a, const uint8_t *seed, uint16_t nonce)
        {
            Bytes xof = shake128_stream(seed, nonce, 3 * P::N * 2);
            size_t ctr = rej_uniform(a, P::N, xof.data(), xof.size());
            if (ctr < P::N)
                throw std::runtime_error("ML-DSA poly_uniform: insufficient XOF output");
        }

        inline size_t rej_eta(int32_t *a, size_t len, const uint8_t *buf, size_t buflen)
        {
            size_t ctr = 0, pos = 0;
            while (ctr < len && pos < buflen)
            {
                uint32_t t0 = buf[pos] & 0x0F;
                uint32_t t1 = buf[pos++] >> 4;
                if (t0 < 9)
                    a[ctr++] = static_cast<int32_t>(P::ETA) - static_cast<int32_t>(t0);
                if (t1 < 9 && ctr < len)
                    a[ctr++] = static_cast<int32_t>(P::ETA) - static_cast<int32_t>(t1);
            }
            return ctr;
        }

        inline void poly_uniform_eta(int32_t *a, const uint8_t *seed, uint16_t nonce)
        {
            Bytes xof = shake256_stream(seed, nonce, 512);
            size_t ctr = rej_eta(a, P::N, xof.data(), xof.size());
            if (ctr < P::N)
                throw std::runtime_error("ML-DSA poly_uniform_eta: insufficient XOF output");
        }

        inline void polyz_unpack(int32_t *r, const uint8_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                r[2 * i + 0] = a[5 * i + 0] | (static_cast<uint32_t>(a[5 * i + 1]) << 8) |
                               (static_cast<uint32_t>(a[5 * i + 2]) << 16);
                r[2 * i + 0] &= 0xFFFFF;
                r[2 * i + 1] = a[5 * i + 2] >> 4 | (static_cast<uint32_t>(a[5 * i + 3]) << 4) |
                               (static_cast<uint32_t>(a[5 * i + 4]) << 12);
                r[2 * i + 0] = static_cast<int32_t>(P::GAMMA1) - r[2 * i + 0];
                r[2 * i + 1] = static_cast<int32_t>(P::GAMMA1) - r[2 * i + 1];
            }
        }

        inline void poly_uniform_gamma1(int32_t *a, const uint8_t *seed, uint16_t nonce)
        {
            Bytes xof = shake256_stream(seed, nonce, P::POLYZ_PACKEDBYTES);
            polyz_unpack(a, xof.data());
        }

        inline void poly_challenge(int32_t *c, const uint8_t *seed)
        {
            Bytes xof = qchain::crypto::shake256(Bytes(seed, seed + P::CTILDEBYTES), 272);
            uint64_t signs = 0;
            for (int i = 0; i < 8; ++i)
                signs |= static_cast<uint64_t>(xof[i]) << 8 * i;
            int pos = 8;
            for (int i = 0; i < P::N; ++i)
                c[i] = 0;
            for (int i = P::N - P::TAU; i < P::N; ++i)
            {
                uint8_t b;
                do
                {
                    b = xof[pos++];
                } while (b > i);
                c[i] = c[b];
                c[b] = 1 - 2 * static_cast<int32_t>(signs & 1);
                signs >>= 1;
            }
        }

        // ─── poly 运算 ────────────────────────────────────────────────
        inline void poly_reduce(int32_t *a) { for (int i = 0; i < P::N; ++i) a[i] = reduce32(a[i]); }
        inline void poly_caddq(int32_t *a) { for (int i = 0; i < P::N; ++i) a[i] = caddq(a[i]); }
        inline void poly_add(int32_t *c, const int32_t *a, const int32_t *b)
        { for (int i = 0; i < P::N; ++i) c[i] = a[i] + b[i]; }
        inline void poly_sub(int32_t *c, const int32_t *a, const int32_t *b)
        { for (int i = 0; i < P::N; ++i) c[i] = a[i] - b[i]; }
        inline void poly_shiftl(int32_t *a) { for (int i = 0; i < P::N; ++i) a[i] <<= P::D; }
        inline void poly_pointwise_montgomery(int32_t *c, const int32_t *a, const int32_t *b)
        { for (int i = 0; i < P::N; ++i) c[i] = montgomery_reduce(static_cast<int64_t>(a[i]) * b[i]); }

        inline void poly_power2round(int32_t *a1, int32_t *a0, const int32_t *a)
        { for (int i = 0; i < P::N; ++i) a1[i] = power2round(&a0[i], a[i]); }
        inline void poly_decompose(int32_t *a1, int32_t *a0, const int32_t *a)
        { for (int i = 0; i < P::N; ++i) a1[i] = decompose(&a0[i], a[i]); }
        inline unsigned int poly_make_hint(int32_t *h, const int32_t *a0, const int32_t *a1)
        {
            unsigned int s = 0;
            for (int i = 0; i < P::N; ++i) { h[i] = make_hint(a0[i], a1[i]); s += h[i]; }
            return s;
        }
        inline void poly_use_hint(int32_t *b, const int32_t *a, const int32_t *h)
        { for (int i = 0; i < P::N; ++i) b[i] = use_hint(a[i], static_cast<unsigned int>(h[i])); }

        inline int poly_chknorm(const int32_t *a, int32_t B)
        {
            if (B > (P::Q - 1) / 8)
                return 1;
            for (int i = 0; i < P::N; ++i)
            {
                int32_t t = a[i] >> 31;
                t = a[i] - (t & 2 * a[i]);
                if (t >= B)
                    return 1;
            }
            return 0;
        }

        // ─── 编码 ─────────────────────────────────────────────────────
        inline void polyeta_pack(uint8_t *r, const int32_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                uint8_t t0 = static_cast<uint8_t>(P::ETA - a[2 * i + 0]);
                uint8_t t1 = static_cast<uint8_t>(P::ETA - a[2 * i + 1]);
                r[i] = static_cast<uint8_t>(t0 | (t1 << 4));
            }
        }
        inline void polyeta_unpack(int32_t *r, const uint8_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                r[2 * i + 0] = a[i] & 0x0F;
                r[2 * i + 1] = a[i] >> 4;
                r[2 * i + 0] = P::ETA - r[2 * i + 0];
                r[2 * i + 1] = P::ETA - r[2 * i + 1];
            }
        }
        inline void polyt1_pack(uint8_t *r, const int32_t *a)
        {
            for (int i = 0; i < P::N / 4; ++i)
            {
                r[5 * i + 0] = static_cast<uint8_t>(a[4 * i + 0] >> 0);
                r[5 * i + 1] = static_cast<uint8_t>((a[4 * i + 0] >> 8) | (a[4 * i + 1] << 2));
                r[5 * i + 2] = static_cast<uint8_t>((a[4 * i + 1] >> 6) | (a[4 * i + 2] << 4));
                r[5 * i + 3] = static_cast<uint8_t>((a[4 * i + 2] >> 4) | (a[4 * i + 3] << 6));
                r[5 * i + 4] = static_cast<uint8_t>(a[4 * i + 3] >> 2);
            }
        }
        inline void polyt1_unpack(int32_t *r, const uint8_t *a)
        {
            for (int i = 0; i < P::N / 4; ++i)
            {
                r[4 * i + 0] = (a[5 * i + 0] >> 0 | (static_cast<uint32_t>(a[5 * i + 1]) << 8)) & 0x3FF;
                r[4 * i + 1] = (a[5 * i + 1] >> 2 | (static_cast<uint32_t>(a[5 * i + 2]) << 6)) & 0x3FF;
                r[4 * i + 2] = (a[5 * i + 2] >> 4 | (static_cast<uint32_t>(a[5 * i + 3]) << 4)) & 0x3FF;
                r[4 * i + 3] = (a[5 * i + 3] >> 6 | (static_cast<uint32_t>(a[5 * i + 4]) << 2)) & 0x3FF;
            }
        }
        inline void polyt0_pack(uint8_t *r, const int32_t *a)
        {
            for (int i = 0; i < P::N / 8; ++i)
            {
                uint32_t t[8];
                for (int j = 0; j < 8; ++j)
                    t[j] = static_cast<uint32_t>((1 << (P::D - 1)) - a[8 * i + j]);
                r[13 * i + 0] = static_cast<uint8_t>(t[0]);
                r[13 * i + 1] = static_cast<uint8_t>(t[0] >> 8);
                r[13 * i + 1] |= static_cast<uint8_t>(t[1] << 5);
                r[13 * i + 2] = static_cast<uint8_t>(t[1] >> 3);
                r[13 * i + 3] = static_cast<uint8_t>(t[1] >> 11);
                r[13 * i + 3] |= static_cast<uint8_t>(t[2] << 2);
                r[13 * i + 4] = static_cast<uint8_t>(t[2] >> 6);
                r[13 * i + 4] |= static_cast<uint8_t>(t[3] << 7);
                r[13 * i + 5] = static_cast<uint8_t>(t[3] >> 1);
                r[13 * i + 6] = static_cast<uint8_t>(t[3] >> 9);
                r[13 * i + 6] |= static_cast<uint8_t>(t[4] << 4);
                r[13 * i + 7] = static_cast<uint8_t>(t[4] >> 4);
                r[13 * i + 8] = static_cast<uint8_t>(t[4] >> 12);
                r[13 * i + 8] |= static_cast<uint8_t>(t[5] << 1);
                r[13 * i + 9] = static_cast<uint8_t>(t[5] >> 7);
                r[13 * i + 9] |= static_cast<uint8_t>(t[6] << 6);
                r[13 * i + 10] = static_cast<uint8_t>(t[6] >> 2);
                r[13 * i + 11] = static_cast<uint8_t>(t[6] >> 10);
                r[13 * i + 11] |= static_cast<uint8_t>(t[7] << 3);
                r[13 * i + 12] = static_cast<uint8_t>(t[7] >> 5);
            }
        }
        inline void polyt0_unpack(int32_t *r, const uint8_t *a)
        {
            for (int i = 0; i < P::N / 8; ++i)
            {
                r[8 * i + 0] = a[13 * i + 0] | (static_cast<uint32_t>(a[13 * i + 1]) << 8);
                r[8 * i + 0] &= 0x1FFF;
                r[8 * i + 1] = a[13 * i + 1] >> 5 | (static_cast<uint32_t>(a[13 * i + 2]) << 3) |
                               (static_cast<uint32_t>(a[13 * i + 3]) << 11);
                r[8 * i + 1] &= 0x1FFF;
                r[8 * i + 2] = a[13 * i + 3] >> 2 | (static_cast<uint32_t>(a[13 * i + 4]) << 6);
                r[8 * i + 2] &= 0x1FFF;
                r[8 * i + 3] = a[13 * i + 4] >> 7 | (static_cast<uint32_t>(a[13 * i + 5]) << 1) |
                               (static_cast<uint32_t>(a[13 * i + 6]) << 9);
                r[8 * i + 3] &= 0x1FFF;
                r[8 * i + 4] = a[13 * i + 6] >> 4 | (static_cast<uint32_t>(a[13 * i + 7]) << 4) |
                               (static_cast<uint32_t>(a[13 * i + 8]) << 12);
                r[8 * i + 4] &= 0x1FFF;
                r[8 * i + 5] = a[13 * i + 8] >> 1 | (static_cast<uint32_t>(a[13 * i + 9]) << 7);
                r[8 * i + 5] &= 0x1FFF;
                r[8 * i + 6] = a[13 * i + 9] >> 6 | (static_cast<uint32_t>(a[13 * i + 10]) << 2) |
                               (static_cast<uint32_t>(a[13 * i + 11]) << 10);
                r[8 * i + 6] &= 0x1FFF;
                r[8 * i + 7] = a[13 * i + 11] >> 3 | (static_cast<uint32_t>(a[13 * i + 12]) << 5);
                r[8 * i + 7] &= 0x1FFF;
                for (int j = 0; j < 8; ++j)
                    r[8 * i + j] = (1 << (P::D - 1)) - r[8 * i + j];
            }
        }
        inline void polyz_pack(uint8_t *r, const int32_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
            {
                uint32_t t0 = static_cast<uint32_t>(P::GAMMA1 - a[2 * i + 0]);
                uint32_t t1 = static_cast<uint32_t>(P::GAMMA1 - a[2 * i + 1]);
                r[5 * i + 0] = static_cast<uint8_t>(t0);
                r[5 * i + 1] = static_cast<uint8_t>(t0 >> 8);
                r[5 * i + 2] = static_cast<uint8_t>(t0 >> 16);
                r[5 * i + 2] |= static_cast<uint8_t>(t1 << 4);
                r[5 * i + 3] = static_cast<uint8_t>(t1 >> 4);
                r[5 * i + 4] = static_cast<uint8_t>(t1 >> 12);
            }
        }
        inline void polyw1_pack(uint8_t *r, const int32_t *a)
        {
            for (int i = 0; i < P::N / 2; ++i)
                r[i] = static_cast<uint8_t>(a[2 * i + 0] | (a[2 * i + 1] << 4));
        }

        // ─── 向量运算（长度 L 和 K）───────────────────────────────────
        inline void polyvec_matrix_expand(int32_t mat[P::K][P::L][P::N], const uint8_t *rho)
        {
            for (int i = 0; i < P::K; ++i)
                for (int j = 0; j < P::L; ++j)
                    poly_uniform(mat[i][j], rho, static_cast<uint16_t>((i << 8) + j));
        }
        inline void polyvecl_ntt(int32_t v[P::L][P::N]) { for (int i = 0; i < P::L; ++i) ntt(v[i]); }
        inline void polyveck_ntt(int32_t v[P::K][P::N]) { for (int i = 0; i < P::K; ++i) ntt(v[i]); }
        inline void polyvecl_invntt_tomont(int32_t v[P::L][P::N]) { for (int i = 0; i < P::L; ++i) invntt_tomont(v[i]); }
        inline void polyveck_invntt_tomont(int32_t v[P::K][P::N]) { for (int i = 0; i < P::K; ++i) invntt_tomont(v[i]); }
        inline void polyvecl_reduce(int32_t v[P::L][P::N]) { for (int i = 0; i < P::L; ++i) poly_reduce(v[i]); }
        inline void polyveck_reduce(int32_t v[P::K][P::N]) { for (int i = 0; i < P::K; ++i) poly_reduce(v[i]); }
        inline void polyveck_caddq(int32_t v[P::K][P::N]) { for (int i = 0; i < P::K; ++i) poly_caddq(v[i]); }
        inline void polyvecl_add(int32_t *w, const int32_t *u, const int32_t *v)
        { for (int i = 0; i < P::L * P::N; ++i) w[i] = u[i] + v[i]; }
        inline void polyveck_add(int32_t *w, const int32_t *u, const int32_t *v)
        { for (int i = 0; i < P::K * P::N; ++i) w[i] = u[i] + v[i]; }
        inline void polyveck_sub(int32_t *w, const int32_t *u, const int32_t *v)
        { for (int i = 0; i < P::K * P::N; ++i) w[i] = u[i] - v[i]; }
        inline void polyveck_shiftl(int32_t v[P::K][P::N]) { for (int i = 0; i < P::K; ++i) poly_shiftl(v[i]); }
        inline void polyvecl_pointwise_poly_montgomery(int32_t r[P::L][P::N], const int32_t *a, const int32_t v[P::L][P::N])
        { for (int i = 0; i < P::L; ++i) poly_pointwise_montgomery(r[i], a, v[i]); }
        inline void polyveck_pointwise_poly_montgomery(int32_t r[P::K][P::N], const int32_t *a, const int32_t v[P::K][P::N])
        { for (int i = 0; i < P::K; ++i) poly_pointwise_montgomery(r[i], a, v[i]); }
        inline void polyvecl_pointwise_acc_montgomery(int32_t *w, const int32_t u[P::L][P::N], const int32_t v[P::L][P::N])
        {
            poly_pointwise_montgomery(w, u[0], v[0]);
            for (int i = 1; i < P::L; ++i)
            {
                int32_t t[P::N];
                poly_pointwise_montgomery(t, u[i], v[i]);
                poly_add(w, w, t);
            }
        }
        inline void polyvec_matrix_pointwise_montgomery(int32_t t[P::K][P::N], const int32_t mat[P::K][P::L][P::N], const int32_t v[P::L][P::N])
        {
            for (int i = 0; i < P::K; ++i)
                polyvecl_pointwise_acc_montgomery(t[i], mat[i], v);
        }
        inline void polyvecl_uniform_eta(int32_t v[P::L][P::N], const uint8_t *seed, uint16_t nonce)
        { for (int i = 0; i < P::L; ++i) poly_uniform_eta(v[i], seed, nonce++); }
        inline void polyveck_uniform_eta(int32_t v[P::K][P::N], const uint8_t *seed, uint16_t nonce)
        { for (int i = 0; i < P::K; ++i) poly_uniform_eta(v[i], seed, nonce++); }
        inline void polyvecl_uniform_gamma1(int32_t v[P::L][P::N], const uint8_t *seed, uint16_t nonce)
        { for (int i = 0; i < P::L; ++i) poly_uniform_gamma1(v[i], seed, static_cast<uint16_t>(P::L * nonce + i)); }
        inline int polyvecl_chknorm(const int32_t v[P::L][P::N], int32_t B)
        { for (int i = 0; i < P::L; ++i) if (poly_chknorm(v[i], B)) return 1; return 0; }
        inline int polyveck_chknorm(const int32_t v[P::K][P::N], int32_t B)
        { for (int i = 0; i < P::K; ++i) if (poly_chknorm(v[i], B)) return 1; return 0; }
        inline void polyveck_power2round(int32_t v1[P::K][P::N], int32_t v0[P::K][P::N], const int32_t v[P::K][P::N])
        { for (int i = 0; i < P::K; ++i) poly_power2round(v1[i], v0[i], v[i]); }
        inline void polyveck_decompose(int32_t v1[P::K][P::N], int32_t v0[P::K][P::N], const int32_t v[P::K][P::N])
        { for (int i = 0; i < P::K; ++i) poly_decompose(v1[i], v0[i], v[i]); }
        inline unsigned int polyveck_make_hint(int32_t h[P::K][P::N], const int32_t v0[P::K][P::N], const int32_t v1[P::K][P::N])
        {
            unsigned int s = 0;
            for (int i = 0; i < P::K; ++i) s += poly_make_hint(h[i], v0[i], v1[i]);
            return s;
        }
        inline void polyveck_use_hint(int32_t w[P::K][P::N], const int32_t u[P::K][P::N], const int32_t h[P::K][P::N])
        { for (int i = 0; i < P::K; ++i) poly_use_hint(w[i], u[i], h[i]); }
        inline void polyveck_pack_w1(uint8_t *r, const int32_t w1[P::K][P::N])
        { for (int i = 0; i < P::K; ++i) polyw1_pack(r + i * P::POLYW1_PACKEDBYTES, w1[i]); }

    // ══════════════════════════════════════════════════════════════════
    // ML-DSA-65（Dilithium）—— 定义于 mldsa_detail 命名空间内，
    // 可直接使用本命名空间的 ntt/poly_* 等辅助函数。
    // ══════════════════════════════════════════════════════════════════
    class MlDsa65
    {
        using P = MlDsaParams;

    public:
        // ── 确定性密钥生成（coins = ζ 32 字节）────────────────────────
        static std::pair<Bytes, Bytes> keygen_derand(const Bytes &coins)
        {
            if (coins.size() != P::SEEDBYTES)
                throw std::invalid_argument("MlDsa65::keygen_derand: coins must be 32 bytes");

            uint8_t seedbuf[2 * P::SEEDBYTES + P::CRHBYTES];
            std::memcpy(seedbuf, coins.data(), P::SEEDBYTES);
            seedbuf[P::SEEDBYTES + 0] = static_cast<uint8_t>(P::K);
            seedbuf[P::SEEDBYTES + 1] = static_cast<uint8_t>(P::L);
            Bytes expanded = qchain::crypto::shake256(
                Bytes(seedbuf, seedbuf + P::SEEDBYTES + 2), 2 * P::SEEDBYTES + P::CRHBYTES);
            const uint8_t *rho = expanded.data();
            const uint8_t *rhoprime = rho + P::SEEDBYTES;
            const uint8_t *key = rhoprime + P::CRHBYTES;

            int32_t mat[P::K][P::L][P::N], s1[P::L][P::N], s2[P::K][P::N], t1[P::K][P::N], t0[P::K][P::N], s1hat[P::L][P::N];

            polyvec_matrix_expand(mat, rho);
            polyvecl_uniform_eta(s1, rhoprime, 0);
            polyveck_uniform_eta(s2, rhoprime, P::L);

            for (int i = 0; i < P::L; ++i) for (int j = 0; j < P::N; ++j) s1hat[i][j] = s1[i][j];
            polyvecl_ntt(s1hat);
            polyvec_matrix_pointwise_montgomery(t1, mat, s1hat);
            polyveck_reduce(t1);
            polyveck_invntt_tomont(t1);
            polyveck_add(reinterpret_cast<int32_t *>(t1), reinterpret_cast<const int32_t *>(t1), reinterpret_cast<const int32_t *>(s2));

            polyveck_caddq(t1);
            polyveck_power2round(t1, t0, t1);

            Bytes pk(P::PK_BYTES);
            std::copy(rho, rho + P::SEEDBYTES, pk.begin());
            for (int i = 0; i < P::K; ++i)
                polyt1_pack(pk.data() + P::SEEDBYTES + i * P::POLYT1_PACKEDBYTES, t1[i]);

            Bytes tr = qchain::crypto::shake256(pk, P::TRBYTES);

            Bytes sk(P::SK_BYTES);
            std::copy(rho, rho + P::SEEDBYTES, sk.begin());
            std::copy(key, key + P::SEEDBYTES, sk.begin() + P::SEEDBYTES);
            std::copy(tr.begin(), tr.end(), sk.begin() + 2 * P::SEEDBYTES);
            size_t off = 2 * P::SEEDBYTES + P::TRBYTES;
            for (int i = 0; i < P::L; ++i)
                polyeta_pack(sk.data() + off + i * P::POLYETA_PACKEDBYTES, s1[i]);
            off += P::L * P::POLYETA_PACKEDBYTES;
            for (int i = 0; i < P::K; ++i)
                polyeta_pack(sk.data() + off + i * P::POLYETA_PACKEDBYTES, s2[i]);
            off += P::K * P::POLYETA_PACKEDBYTES;
            for (int i = 0; i < P::K; ++i)
                polyt0_pack(sk.data() + off + i * P::POLYT0_PACKEDBYTES, t0[i]);

            return {pk, sk};
        }

        static std::pair<Bytes, Bytes> keygen()
        {
            qchain::Csprng csprng;
            return keygen_derand(csprng.random_bytes(P::SEEDBYTES));
        }

        // ── 签名（确定性版本，rnd 传全零）────────────────────────────
        static Bytes sign_derand(const Bytes &sk_bytes, const Bytes &msg, const Bytes &rnd)
        {
            if (sk_bytes.size() != P::SK_BYTES)
                throw std::invalid_argument("MlDsa65::sign: bad sk size");
            if (rnd.size() != P::SEEDBYTES)
                throw std::invalid_argument("MlDsa65::sign: rnd must be 32 bytes");

            const uint8_t *rho = sk_bytes.data();
            const uint8_t *key = sk_bytes.data() + P::SEEDBYTES;
            const uint8_t *tr = sk_bytes.data() + 2 * P::SEEDBYTES;
            const uint8_t *sk_s1 = sk_bytes.data() + 2 * P::SEEDBYTES + P::TRBYTES;
            const uint8_t *sk_s2 = sk_s1 + P::L * P::POLYETA_PACKEDBYTES;
            const uint8_t *sk_t0 = sk_s2 + P::K * P::POLYETA_PACKEDBYTES;

            int32_t t0[P::K][P::N], s1[P::L][P::N], s2[P::K][P::N];
            for (int i = 0; i < P::L; ++i) polyeta_unpack(s1[i], sk_s1 + i * P::POLYETA_PACKEDBYTES);
            for (int i = 0; i < P::K; ++i) polyeta_unpack(s2[i], sk_s2 + i * P::POLYETA_PACKEDBYTES);
            for (int i = 0; i < P::K; ++i) polyt0_unpack(t0[i], sk_t0 + i * P::POLYT0_PACKEDBYTES);

            // mu = CRH(tr || pre || msg)，pre = (0, 0)（空 ctx）
            uint8_t pre[2] = {0, 0};
            Bytes mu_in(tr, tr + P::TRBYTES);
            mu_in.insert(mu_in.end(), pre, pre + 2);
            mu_in.insert(mu_in.end(), msg.begin(), msg.end());
            Bytes mu = qchain::crypto::shake256(mu_in, P::CRHBYTES);

            // rhoprime = CRH(key || rnd || mu)
            Bytes rp_in(key, key + P::SEEDBYTES);
            rp_in.insert(rp_in.end(), rnd.begin(), rnd.end());
            rp_in.insert(rp_in.end(), mu.begin(), mu.end());
            Bytes rhoprime = qchain::crypto::shake256(rp_in, P::CRHBYTES);

            int32_t mat[P::K][P::L][P::N];
            polyvec_matrix_expand(mat, rho);
            polyvecl_ntt(s1);
            polyveck_ntt(s2);
            polyveck_ntt(t0);

            Bytes sig(P::SIG_BYTES);
            uint16_t nonce = 0;
            for (;;)
            {
                int32_t y[P::L][P::N], z[P::L][P::N], w1[P::K][P::N], w0[P::K][P::N], h[P::K][P::N], cp[P::N];
                polyvecl_uniform_gamma1(y, rhoprime.data(), nonce++);

                for (int i = 0; i < P::L; ++i) for (int j = 0; j < P::N; ++j) z[i][j] = y[i][j];
                polyvecl_ntt(z);
                polyvec_matrix_pointwise_montgomery(w1, mat, z);
                polyveck_reduce(w1);
                polyveck_invntt_tomont(w1);

                polyveck_caddq(w1);
                polyveck_decompose(w1, w0, w1);
                polyveck_pack_w1(sig.data(), w1);

                Bytes c_in(mu.begin(), mu.end());
                c_in.insert(c_in.end(), sig.begin(), sig.begin() + P::K * P::POLYW1_PACKEDBYTES);
                Bytes ctilde = qchain::crypto::shake256(c_in, P::CTILDEBYTES);
                poly_challenge(cp, ctilde.data());
                ntt(cp);

                polyvecl_pointwise_poly_montgomery(z, cp, s1);
                polyvecl_invntt_tomont(z);
                polyvecl_add(reinterpret_cast<int32_t *>(z), reinterpret_cast<const int32_t *>(z), reinterpret_cast<const int32_t *>(y));
                polyvecl_reduce(z);
                if (polyvecl_chknorm(z, static_cast<int32_t>(P::GAMMA1 - P::BETA)))
                    continue;

                polyveck_pointwise_poly_montgomery(h, cp, s2);
                polyveck_invntt_tomont(h);
                polyveck_sub(reinterpret_cast<int32_t *>(w0), reinterpret_cast<const int32_t *>(w0), reinterpret_cast<const int32_t *>(h));
                polyveck_reduce(w0);
                if (polyveck_chknorm(w0, static_cast<int32_t>(P::GAMMA2 - P::BETA)))
                    continue;

                polyveck_pointwise_poly_montgomery(h, cp, t0);
                polyveck_invntt_tomont(h);
                polyveck_reduce(h);
                if (polyveck_chknorm(h, static_cast<int32_t>(P::GAMMA2)))
                    continue;

                polyveck_add(reinterpret_cast<int32_t *>(w0), reinterpret_cast<const int32_t *>(w0), reinterpret_cast<const int32_t *>(h));
                unsigned int n = polyveck_make_hint(h, w0, w1);
                if (n > static_cast<unsigned int>(P::OMEGA))
                    continue;

                // pack_sig：c || z || h
                std::copy(ctilde.begin(), ctilde.end(), sig.begin());
                for (int i = 0; i < P::L; ++i)
                    polyz_pack(sig.data() + P::CTILDEBYTES + i * P::POLYZ_PACKEDBYTES, z[i]);
                size_t h_off = P::CTILDEBYTES + P::L * P::POLYZ_PACKEDBYTES;
                for (int i = 0; i < P::OMEGA + P::K; ++i)
                    sig[h_off + i] = 0;
                int kk = 0;
                for (int i = 0; i < P::K; ++i)
                {
                    for (int j = 0; j < P::N; ++j)
                        if (h[i][j] != 0)
                            sig[h_off + kk++] = static_cast<uint8_t>(j);
                    sig[h_off + P::OMEGA + i] = static_cast<uint8_t>(kk);
                }
                return sig;
            }
        }

        static Bytes sign(const Bytes &sk, const Bytes &msg)
        {
            return sign_derand(sk, msg, Bytes(P::SEEDBYTES, 0)); // 确定性签名
        }

        // ── 验证 ─────────────────────────────────────────────────────
        static bool verify(const Bytes &pk_bytes, const Bytes &msg, const Bytes &sig)
        {
            if (pk_bytes.size() != P::PK_BYTES || sig.size() != P::SIG_BYTES)
                return false;

            const uint8_t *rho = pk_bytes.data();
            int32_t t1[P::K][P::N], z[P::L][P::N], h[P::K][P::N];
            for (int i = 0; i < P::K; ++i)
                polyt1_unpack(t1[i], pk_bytes.data() + P::SEEDBYTES + i * P::POLYT1_PACKEDBYTES);

            // unpack_sig
            uint8_t c[P::CTILDEBYTES];
            std::copy(sig.begin(), sig.begin() + P::CTILDEBYTES, c);
            for (int i = 0; i < P::L; ++i)
                polyz_unpack(z[i], sig.data() + P::CTILDEBYTES + i * P::POLYZ_PACKEDBYTES);
            size_t h_off = P::CTILDEBYTES + P::L * P::POLYZ_PACKEDBYTES;
            int kk = 0;
            for (int i = 0; i < P::K; ++i)
            {
                for (int j = 0; j < P::N; ++j)
                    h[i][j] = 0;
                if (sig[h_off + P::OMEGA + i] < kk || sig[h_off + P::OMEGA + i] > P::OMEGA)
                    return false;
                for (int j = kk; j < sig[h_off + P::OMEGA + i]; ++j)
                {
                    if (j > kk && sig[h_off + j] <= sig[h_off + j - 1])
                        return false;
                    h[i][sig[h_off + j]] = 1;
                }
                kk = sig[h_off + P::OMEGA + i];
            }
            for (int j = kk; j < P::OMEGA; ++j)
                if (sig[h_off + j])
                    return false;

            if (polyvecl_chknorm(z, static_cast<int32_t>(P::GAMMA1 - P::BETA)))
                return false;

            uint8_t pre[2] = {0, 0};
            Bytes mu = qchain::crypto::shake256(pk_bytes, P::TRBYTES);
            Bytes mu_in(mu.begin(), mu.end());
            mu_in.insert(mu_in.end(), pre, pre + 2);
            mu_in.insert(mu_in.end(), msg.begin(), msg.end());
            mu = qchain::crypto::shake256(mu_in, P::CRHBYTES);

            int32_t cp[P::N], mat[P::K][P::L][P::N], w1[P::K][P::N];
            poly_challenge(cp, c);
            polyvec_matrix_expand(mat, rho);

            polyvecl_ntt(z);
            polyvec_matrix_pointwise_montgomery(w1, mat, z);

            ntt(cp);
            polyveck_shiftl(t1);
            polyveck_ntt(t1);
            polyveck_pointwise_poly_montgomery(t1, cp, t1);

            polyveck_sub(reinterpret_cast<int32_t *>(w1), reinterpret_cast<const int32_t *>(w1), reinterpret_cast<const int32_t *>(t1));
            polyveck_reduce(w1);
            polyveck_invntt_tomont(w1);

            polyveck_caddq(w1);
            polyveck_use_hint(w1, w1, h);

            uint8_t buf[P::K * P::POLYW1_PACKEDBYTES];
            polyveck_pack_w1(buf, w1);

            Bytes c_in(mu.begin(), mu.end());
            c_in.insert(c_in.end(), buf, buf + P::K * P::POLYW1_PACKEDBYTES);
            Bytes c2 = qchain::crypto::shake256(c_in, P::CTILDEBYTES);

            return qchain::constant_time_equal(c, c2.data(), P::CTILDEBYTES);
        }
    };

    }

    // 导出 MlDsa65 到 qchain::pqc 命名空间。
    using mldsa_detail::MlDsa65;

}