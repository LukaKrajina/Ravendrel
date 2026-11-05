#pragma once
// ─── 轻量 AES-256（header-only，无外部依赖，跨平台）────────────────
// 提供 AES-256-CBC 与 AES-256-GCM（认证加密）。用于 .rvdlp 插件包。
#include <cstdint>
#include <cstring>
#include <vector>
#include <chrono>

namespace rvdlrsp::editor
{

    // ─── AES 核心（CBC/GCM 共用）────────────────────────────────────
    namespace aes_detail
    {
        inline const uint8_t sbox[256] = {
            0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
            0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
            0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
            0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
            0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
            0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
            0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
            0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
            0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
            0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
            0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
            0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
            0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
            0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
            0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
            0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

        constexpr int kRounds = 14; // AES-256

        inline void key_expand(const uint8_t key[32], uint8_t *rk)
        {
            const int nk = 8, nr = kRounds, total = (nr + 1) * 16;
            std::memcpy(rk, key, 32);
            int generated = 32;
            uint8_t temp[4];
            int rcon = 1;
            while (generated < total)
            {
                for (int i = 0; i < 4; ++i)
                    temp[i] = rk[generated - 4 + i];
                if (generated % (nk * 4) == 0)
                {
                    uint8_t t0 = temp[0];
                    temp[0] = sbox[temp[1]] ^ rcon;
                    temp[1] = sbox[temp[2]];
                    temp[2] = sbox[temp[3]];
                    temp[3] = sbox[t0];
                    rcon = (rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0);
                }
                else if (nk > 6 && generated % (nk * 4) == 16)
                {
                    for (int i = 0; i < 4; ++i)
                        temp[i] = sbox[temp[i]];
                }
                for (int i = 0; i < 4; ++i)
                {
                    rk[generated] = rk[generated - nk * 4] ^ temp[i];
                    ++generated;
                }
            }
        }

        inline void add_round_key(uint8_t *s, const uint8_t *rk)
        {
            for (int i = 0; i < 16; ++i)
                s[i] ^= rk[i];
        }

        inline void sub_bytes(uint8_t *s)
        {
            for (int i = 0; i < 16; ++i)
                s[i] = sbox[s[i]];
        }

        inline void inv_sub_bytes(uint8_t *s)
        {
            static const uint8_t inv[256] = {
                0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
                0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
                0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
                0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
                0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
                0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
                0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
                0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
                0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
                0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
                0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
                0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
                0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
                0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
                0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
                0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};
            for (int i = 0; i < 16; ++i)
                s[i] = inv[s[i]];
        }

        inline void shift_rows(uint8_t *s)
        {
            uint8_t t[16];
            for (int i = 0; i < 16; ++i)
                t[i] = s[i];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    s[r + 4 * c] = t[r + 4 * ((c + r) % 4)];
        }

        inline void inv_shift_rows(uint8_t *s)
        {
            uint8_t t[16];
            for (int i = 0; i < 16; ++i)
                t[i] = s[i];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    s[r + 4 * ((c + r) % 4)] = t[r + 4 * c];
        }

        inline uint8_t xtime(uint8_t x) { return (x << 1) ^ ((x & 0x80) ? 0x1b : 0); }

        inline void mix_columns(uint8_t *s)
        {
            for (int c = 0; c < 4; ++c)
            {
                uint8_t a0 = s[c * 4 + 0], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                s[c * 4 + 0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
                s[c * 4 + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
                s[c * 4 + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
                s[c * 4 + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
            }
        }

        inline uint8_t gmul(uint8_t a, uint8_t b)
        {
            uint8_t p = 0;
            for (int i = 0; i < 8; ++i)
            {
                if (b & 1)
                    p ^= a;
                bool hi = a & 0x80;
                a <<= 1;
                if (hi)
                    a ^= 0x1b;
                b >>= 1;
            }
            return p;
        }

        inline void inv_mix_columns(uint8_t *s)
        {
            for (int c = 0; c < 4; ++c)
            {
                uint8_t a0 = s[c * 4 + 0], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                s[c * 4 + 0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                s[c * 4 + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                s[c * 4 + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                s[c * 4 + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        }

        // 加密单块
        inline void encrypt_block(uint8_t *s, const uint8_t *rk)
        {
            add_round_key(s, rk);
            for (int round = 1; round < kRounds; ++round)
            {
                sub_bytes(s);
                shift_rows(s);
                mix_columns(s);
                add_round_key(s, rk + round * 16);
            }
            sub_bytes(s);
            shift_rows(s);
            add_round_key(s, rk + kRounds * 16);
        }

        inline void decrypt_block(const uint8_t *in, uint8_t *s, const uint8_t *rk)
        {
            std::memcpy(s, in, 16);
            add_round_key(s, rk + kRounds * 16);
            for (int round = kRounds - 1; round >= 1; --round)
            {
                inv_shift_rows(s);
                inv_sub_bytes(s);
                add_round_key(s, rk + round * 16);
                inv_mix_columns(s);
            }
            inv_shift_rows(s);
            inv_sub_bytes(s);
            add_round_key(s, rk);
        }

        // ── GCM 用：GF(2^128) 乘法（GHASH）─────────────────────────
        inline void gf_mult(uint8_t *x, const uint8_t *y)
        {
            uint8_t z[16] = {0};
            uint8_t v[16];
            std::memcpy(v, y, 16);
            for (int i = 0; i < 128; ++i)
            {
                if (x[i / 8] & (1 << (7 - (i % 8))))
                    for (int j = 0; j < 16; ++j)
                        z[j] ^= v[j];
                bool lsb = v[15] & 1;
                for (int j = 15; j >= 0; --j)
                {
                    v[j] = v[j] >> 1;
                    if (j > 0 && (v[j - 1] & 1))
                        v[j] |= 0x80;
                }
                if (lsb)
                    v[0] ^= 0xe1;
            }
            std::memcpy(x, z, 16);
        }

        // GHASH：对 data（已按 16 字节对齐的块序列）计算 MAC
        inline void ghash(uint8_t *X, const uint8_t *H, const uint8_t *data, size_t nbytes)
        {
            for (size_t off = 0; off < nbytes; off += 16)
            {
                for (int i = 0; i < 16; ++i)
                    X[i] ^= data[off + i];
                gf_mult(X, H);
            }
        }

        // 32 位大端递增（GCM 的 inc32，作用于块的后 4 字节）
        inline void inc32(uint8_t *block)
        {
            for (int i = 15; i >= 12; --i)
            {
                if (++block[i])
                    break;
            }
        }
    }

    // ─── AES-256-CBC（PKCS7）────────────────────────────────────────
    class Aes256Cbc
    {
    public:
        static constexpr size_t kBlockSize = 16;
        static constexpr size_t kKeySize = 32;

        static std::vector<uint8_t> encrypt(const std::vector<uint8_t> &plain,
                                            const uint8_t key[kKeySize])
        {
            std::vector<uint8_t> padded = pkcs7_pad(plain);
            uint8_t iv[kBlockSize];
            gen_iv(iv);

            std::vector<uint8_t> out;
            out.reserve(kBlockSize + padded.size());
            out.insert(out.end(), iv, iv + kBlockSize);

            uint8_t rk[240];
            aes_detail::key_expand(key, rk);

            uint8_t prev[kBlockSize];
            std::memcpy(prev, iv, kBlockSize);
            for (size_t off = 0; off < padded.size(); off += kBlockSize)
            {
                uint8_t block[kBlockSize];
                for (int i = 0; i < kBlockSize; ++i)
                    block[i] = padded[off + i] ^ prev[i];
                aes_detail::encrypt_block(block, rk);
                out.insert(out.end(), block, block + kBlockSize);
                std::memcpy(prev, block, kBlockSize);
            }
            return out;
        }

        static bool decrypt(const std::vector<uint8_t> &data,
                            const uint8_t key[kKeySize],
                            std::vector<uint8_t> &plain)
        {
            if (data.size() < kBlockSize * 2 || (data.size() % kBlockSize) != 0)
                return false;

            uint8_t rk[240];
            aes_detail::key_expand(key, rk);

            uint8_t iv[kBlockSize];
            std::memcpy(iv, data.data(), kBlockSize);

            std::vector<uint8_t> dec;
            dec.resize(data.size() - kBlockSize);
            uint8_t prev[kBlockSize];
            std::memcpy(prev, iv, kBlockSize);
            for (size_t off = kBlockSize; off < data.size(); off += kBlockSize)
            {
                uint8_t block[kBlockSize];
                std::memcpy(block, data.data() + off, kBlockSize);
                uint8_t decrypted[kBlockSize];
                aes_detail::decrypt_block(block, decrypted, rk);
                for (int i = 0; i < kBlockSize; ++i)
                    dec[off - kBlockSize + i] = decrypted[i] ^ prev[i];
                std::memcpy(prev, block, kBlockSize);
            }
            return pkcs7_unpad(dec, plain);
        }

    private:
        static void gen_iv(uint8_t iv[kBlockSize])
        {
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            for (int i = 0; i < kBlockSize; ++i)
                iv[i] = static_cast<uint8_t>((now >> ((i % 8) * 8)) ^ (i * 0x9e) ^ 0x37);
        }

        static std::vector<uint8_t> pkcs7_pad(const std::vector<uint8_t> &in)
        {
            size_t pad = kBlockSize - (in.size() % kBlockSize);
            std::vector<uint8_t> out = in;
            out.insert(out.end(), pad, static_cast<uint8_t>(pad));
            return out;
        }

        static bool pkcs7_unpad(const std::vector<uint8_t> &in, std::vector<uint8_t> &out)
        {
            if (in.empty())
                return false;
            size_t pad = in.back();
            if (pad == 0 || pad > kBlockSize || pad > in.size())
                return false;
            for (size_t i = 0; i < pad; ++i)
                if (in[in.size() - 1 - i] != pad)
                    return false;
            out.assign(in.begin(), in.end() - pad);
            return true;
        }
    };

    // ─── AES-256-GCM（认证加密）────────────────────────────────────
    // 加密输出格式：[nonce(12) || 密文 || tag(16)]；解密时校验 tag 防篡改。
    class Aes256Gcm
    {
    public:
        static constexpr size_t kKeySize = 32;
        static constexpr size_t kNonceSize = 12;
        static constexpr size_t kTagSize = 16;

        static std::vector<uint8_t> encrypt(const std::vector<uint8_t> &plain,
                                            const uint8_t key[kKeySize])
        {
            uint8_t nonce[kNonceSize];
            gen_nonce(nonce);
            return encrypt_with_nonce(plain, key, nonce);
        }

        static std::vector<uint8_t> encrypt_with_nonce(const std::vector<uint8_t> &plain,
                                                       const uint8_t key[kKeySize],
                                                       const uint8_t nonce[kNonceSize])
        {
            uint8_t rk[240];
            aes_detail::key_expand(key, rk);

            // H = E(K, 0^128)
            uint8_t H[16] = {0};
            aes_detail::encrypt_block(H, rk);

            // J0 = nonce || 0^31 || 1
            uint8_t J0[16];
            std::memcpy(J0, nonce, kNonceSize);
            J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

            // 密文 = GCTR(inc32(J0), P)
            std::vector<uint8_t> cipher(plain.size());
            uint8_t ctr[16];
            std::memcpy(ctr, J0, 16);
            aes_detail::inc32(ctr);
            for (size_t off = 0; off < plain.size(); off += 16)
            {
                uint8_t ks[16];
                std::memcpy(ks, ctr, 16);
                aes_detail::encrypt_block(ks, rk);
                size_t n = std::min<size_t>(16, plain.size() - off);
                for (size_t i = 0; i < n; ++i)
                    cipher[off + i] = plain[off + i] ^ ks[i];
                aes_detail::inc32(ctr);
            }

            // S = GHASH_H(密文 || 0^u || [len(A)=0]64 || [len(C)]64)
            uint8_t S[16] = {0};
            // 密文 padding 到 16 字节对齐后再 GHASH
            std::vector<uint8_t> padded((cipher.size() + 15) / 16 * 16, 0);
            std::memcpy(padded.data(), cipher.data(), cipher.size());
            aes_detail::ghash(S, H, padded.data(), padded.size());
            // 追加长度块（AAD 长度 0 + 密文长度）
            uint8_t lenblock[16] = {0};
            uint64_t clen_bits = static_cast<uint64_t>(cipher.size()) * 8;
            for (int i = 0; i < 8; ++i)
                lenblock[8 + i] = static_cast<uint8_t>(clen_bits >> (56 - i * 8));
            aes_detail::ghash(S, H, lenblock, 16);

            // T = MSB_16(E(K, J0) XOR S)
            uint8_t ej[16];
            std::memcpy(ej, J0, 16);
            aes_detail::encrypt_block(ej, rk);
            uint8_t tag[16];
            for (int i = 0; i < 16; ++i)
                tag[i] = ej[i] ^ S[i];

            std::vector<uint8_t> out;
            out.reserve(kNonceSize + cipher.size() + kTagSize);
            out.insert(out.end(), nonce, nonce + kNonceSize);
            out.insert(out.end(), cipher.begin(), cipher.end());
            out.insert(out.end(), tag, tag + kTagSize);
            return out;
        }

        // 输入 [nonce || 密文 || tag]，验证 tag 并返回明文
        static bool decrypt(const std::vector<uint8_t> &data,
                            const uint8_t key[kKeySize],
                            std::vector<uint8_t> &plain)
        {
            if (data.size() < kNonceSize + kTagSize)
                return false;
            size_t ctlen = data.size() - kNonceSize - kTagSize;

            uint8_t rk[240];
            aes_detail::key_expand(key, rk);

            const uint8_t *nonce = data.data();
            const uint8_t *cipher = data.data() + kNonceSize;
            const uint8_t *tag = data.data() + kNonceSize + ctlen;

            uint8_t H[16] = {0};
            aes_detail::encrypt_block(H, rk);

            uint8_t J0[16];
            std::memcpy(J0, nonce, kNonceSize);
            J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

            // 重算 S
            uint8_t S[16] = {0};
            std::vector<uint8_t> padded((ctlen + 15) / 16 * 16, 0);
            std::memcpy(padded.data(), cipher, ctlen);
            aes_detail::ghash(S, H, padded.data(), padded.size());
            uint8_t lenblock[16] = {0};
            uint64_t clen_bits = static_cast<uint64_t>(ctlen) * 8;
            for (int i = 0; i < 8; ++i)
                lenblock[8 + i] = static_cast<uint8_t>(clen_bits >> (56 - i * 8));
            aes_detail::ghash(S, H, lenblock, 16);

            uint8_t ej[16];
            std::memcpy(ej, J0, 16);
            aes_detail::encrypt_block(ej, rk);
            uint8_t expect[16];
            for (int i = 0; i < 16; ++i)
                expect[i] = ej[i] ^ S[i];

            // 常数时间比较 tag（防时序侧信道）
            uint8_t diff = 0;
            for (int i = 0; i < 16; ++i)
                diff |= expect[i] ^ tag[i];
            if (diff != 0)
                return false;

            // 解密（GCTR）
            plain.resize(ctlen);
            uint8_t ctr[16];
            std::memcpy(ctr, J0, 16);
            aes_detail::inc32(ctr);
            for (size_t off = 0; off < ctlen; off += 16)
            {
                uint8_t ks[16];
                std::memcpy(ks, ctr, 16);
                aes_detail::encrypt_block(ks, rk);
                size_t n = std::min<size_t>(16, ctlen - off);
                for (size_t i = 0; i < n; ++i)
                    plain[off + i] = cipher[off + i] ^ ks[i];
                aes_detail::inc32(ctr);
            }
            return true;
        }

    private:
        static void gen_nonce(uint8_t nonce[kNonceSize])
        {
            auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            for (int i = 0; i < kNonceSize; ++i)
                nonce[i] = static_cast<uint8_t>((now >> ((i % 8) * 8)) ^ (i * 0x5b) ^ 0xa3);
        }
    };
}