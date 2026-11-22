#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <random>

namespace qhal::qcrypt
{
    using Bytes = std::vector<uint8_t>;

    inline uint32_t load32_le(const uint8_t *p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    inline void store32_le(uint8_t *p, uint32_t v)
    {
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)((v >> 8) & 0xFF);
        p[2] = (uint8_t)((v >> 16) & 0xFF);
        p[3] = (uint8_t)((v >> 24) & 0xFF);
    }

    inline Bytes sha256(const Bytes &data)
    {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

        uint64_t bit_len = (uint64_t)data.size() * 8;
        size_t padded_len = ((data.size() + 8) / 64 + 1) * 64;
        Bytes padded(padded_len, 0);
        if (!data.empty())
            std::memcpy(padded.data(), data.data(), data.size());
        padded[data.size()] = 0x80;
        for (int i = 0; i < 8; ++i)
            padded[padded_len - 8 + i] = (uint8_t)(bit_len >> (56 - 8 * i));

        for (size_t off = 0; off < padded_len; off += 64)
        {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = ((uint32_t)padded[off + i * 4] << 24) | ((uint32_t)padded[off + i * 4 + 1] << 16) |
                       ((uint32_t)padded[off + i * 4 + 2] << 8) | (uint32_t)padded[off + i * 4 + 3];
            for (int i = 16; i < 64; ++i)
            {
                uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
                uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i)
            {
                uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
                uint32_t ch = (e & f) ^ (~e & g);
                uint32_t t1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t t2 = S0 + maj;
                hh = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        Bytes out(32);
        for (int i = 0; i < 8; ++i)
        {
            out[i * 4] = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)h[i];
        }
        return out;
    }

    inline Bytes hmac_sha256(const Bytes &key, const Bytes &msg)
    {
        constexpr size_t BLOCK = 64;
        Bytes k = key;
        if (k.size() > BLOCK)
            k = sha256(k);
        k.resize(BLOCK, 0);

        Bytes inner(BLOCK + msg.size());
        for (size_t i = 0; i < BLOCK; ++i)
            inner[i] = (uint8_t)(k[i] ^ 0x36);
        std::memcpy(inner.data() + BLOCK, msg.data(), msg.size());
        Bytes inner_hash = sha256(inner);

        Bytes outer(BLOCK + 32);
        for (size_t i = 0; i < BLOCK; ++i)
            outer[i] = (uint8_t)(k[i] ^ 0x5c);
        std::memcpy(outer.data() + BLOCK, inner_hash.data(), 32);
        return sha256(outer);
    }

    inline Bytes hkdf_sha256(const Bytes &ikm, const Bytes &salt, const Bytes &info, size_t out_len)
    {
        Bytes actual_salt = salt.empty() ? Bytes(32, 0) : salt;
        Bytes prk = hmac_sha256(actual_salt, ikm);

        Bytes okm;
        okm.reserve(out_len);
        Bytes prev;
        uint8_t counter = 1;
        while (okm.size() < out_len)
        {
            Bytes block = prev;
            block.insert(block.end(), info.begin(), info.end());
            block.push_back(counter++);
            Bytes t = hmac_sha256(prk, block);
            okm.insert(okm.end(), t.begin(), t.end());
            prev = t;
        }
        okm.resize(out_len);
        return okm;
    }

    inline Bytes chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                              const Bytes &data, uint32_t counter = 0)
    {
        uint32_t k[8], n[3];
        for (int i = 0; i < 8; ++i)
            k[i] = load32_le(key + 4 * i);
        for (int i = 0; i < 3; ++i)
            n[i] = load32_le(nonce + 4 * i);

        static const uint32_t constants[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};

        auto rotl = [](uint32_t x, int s) { return (x << s) | (x >> (32 - s)); };
        auto quarter = [&](uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d)
        {
            a += b; d ^= a; d = rotl(d, 16);
            c += d; b ^= c; b = rotl(b, 12);
            a += b; d ^= a; d = rotl(d, 8);
            c += d; b ^= c; b = rotl(b, 7);
        };

        Bytes out(data.size());
        uint32_t ctr = counter;
        size_t pos = 0;
        while (pos < data.size())
        {
            uint32_t x[16];
            for (int i = 0; i < 4; ++i)
                x[i] = constants[i];
            for (int i = 0; i < 8; ++i)
                x[4 + i] = k[i];
            x[12] = ctr;
            for (int i = 0; i < 3; ++i)
                x[13 + i] = n[i];

            uint32_t y[16];
            for (int i = 0; i < 16; ++i)
                y[i] = x[i];
            for (int r = 0; r < 10; ++r)
            {
                quarter(y[0], y[4], y[8], y[12]);
                quarter(y[1], y[5], y[9], y[13]);
                quarter(y[2], y[6], y[10], y[14]);
                quarter(y[3], y[7], y[11], y[15]);
                quarter(y[0], y[5], y[10], y[15]);
                quarter(y[1], y[6], y[11], y[12]);
                quarter(y[2], y[7], y[8], y[13]);
                quarter(y[3], y[4], y[9], y[14]);
            }
            for (int i = 0; i < 16; ++i)
                y[i] += x[i];

            for (int i = 0; i < 16 && pos < data.size(); ++i)
            {
                for (int byte = 0; byte < 4 && pos < data.size(); ++byte)
                {
                    out[pos] = (uint8_t)(data[pos] ^ ((y[i] >> (8 * byte)) & 0xFF));
                    ++pos;
                }
            }
            ++ctr;
        }
        return out;
    }

    inline const uint8_t *master_salt()
    {
        static const uint8_t s[32] = {
            0x51, 0x55, 0x41, 0x52, 0x4b, 0x2d, 0x51, 0x4d,
            0x4d, 0x49, 0x2d, 0x51, 0x4f, 0x42, 0x46, 0x2d,
            0x56, 0x32, 0x2d, 0x4d, 0x41, 0x53, 0x54, 0x45,
            0x52, 0x2d, 0x53, 0x41, 0x4c, 0x54, 0x2d, 0x4b
        };
        return s;
    }
    inline const char *kdf_info() { return "rvdl-mmi-obf-v2"; }

    inline Bytes derive_module_key(const std::string &module_name, const Bytes &kdf_salt)
    {
        const uint8_t *salt = master_salt();
        Bytes ikm(salt, salt + 32);
        ikm.insert(ikm.end(), module_name.begin(), module_name.end());
        const char *info = kdf_info();
        Bytes info_bytes(info, info + std::strlen(info));
        return hkdf_sha256(ikm, kdf_salt, info_bytes, 64);
    }

    inline Bytes pack_mmi_v2(const std::string &module_name, const Bytes &binary_payload)
    {
        std::random_device rd;
        Bytes nonce(12), kdf_salt(16);
        for (auto &b : nonce)
            b = (uint8_t)rd();
        for (auto &b : kdf_salt)
            b = (uint8_t)rd();

        Bytes key = derive_module_key(module_name, kdf_salt);
        Bytes ciphertext = chacha20_xor(key.data(), nonce.data(), binary_payload);

        Bytes mac_msg = nonce;
        mac_msg.insert(mac_msg.end(), ciphertext.begin(), ciphertext.end());
        Bytes tag = hmac_sha256(Bytes(key.begin() + 32, key.end()), mac_msg);

        Bytes out;
        out.reserve(16 + module_name.size() + 32 + ciphertext.size() + 32);
        const char magic[4] = {'R', 'V', 'D', 'L'};
        out.insert(out.end(), magic, magic + 4);
        uint8_t hdr[8];
        store32_le(hdr, 2);
        store32_le(hdr + 4, 0);
        out.insert(out.end(), hdr, hdr + 8);
        uint16_t nl = (uint16_t)module_name.size();
        out.push_back((uint8_t)(nl & 0xFF));
        out.push_back((uint8_t)(nl >> 8));
        out.insert(out.end(), module_name.begin(), module_name.end());
        out.insert(out.end(), nonce.begin(), nonce.end());
        out.insert(out.end(), kdf_salt.begin(), kdf_salt.end());
        uint8_t len[4];
        store32_le(len, (uint32_t)ciphertext.size());
        out.insert(out.end(), len, len + 4);
        out.insert(out.end(), ciphertext.begin(), ciphertext.end());
        out.insert(out.end(), tag.begin(), tag.end());
        return out;
    }

    inline Bytes unpack_mmi_v2(const Bytes &file_data)
    {
        if (file_data.size() < 14)
            throw std::runtime_error("Invalid .rvdli: file too short");
        if (file_data[0] != 'R' || file_data[1] != 'V' || file_data[2] != 'D' || file_data[3] != 'L')
            throw std::runtime_error("Invalid .rvdli: bad magic");

        uint32_t version = load32_le(file_data.data() + 4);
        if (version != 2)
            throw std::runtime_error("Unsupported .rvdli version " + std::to_string(version));

        uint32_t flags = load32_le(file_data.data() + 8);
        if (flags != 0)
            throw std::runtime_error("Unsupported .rvdli encryption flags");

        size_t off = 12;
        uint16_t nl = (uint16_t)file_data[off] | ((uint16_t)file_data[off + 1] << 8);
        off += 2;
        if (off + nl > file_data.size())
            throw std::runtime_error("Invalid .rvdli: bad name length");
        std::string module_name(file_data.begin() + off, file_data.begin() + off + nl);
        off += nl;

        if (off + 12 + 16 + 4 > file_data.size())
            throw std::runtime_error("Invalid .rvdli: file too short");
        Bytes nonce(file_data.begin() + off, file_data.begin() + off + 12);
        off += 12;
        Bytes kdf_salt(file_data.begin() + off, file_data.begin() + off + 16);
        off += 16;
        uint32_t ct_len = load32_le(file_data.data() + off);
        off += 4;
        if (off + ct_len + 32 != file_data.size())
            throw std::runtime_error("Invalid .rvdli: bad ciphertext length");

        Bytes ciphertext(file_data.begin() + off, file_data.begin() + off + ct_len);
        Bytes tag(file_data.begin() + off + ct_len, file_data.end());

        Bytes key = derive_module_key(module_name, kdf_salt);
        Bytes mac_msg = nonce;
        mac_msg.insert(mac_msg.end(), ciphertext.begin(), ciphertext.end());
        Bytes expect = hmac_sha256(Bytes(key.begin() + 32, key.end()), mac_msg);

        if (expect.size() != tag.size())
            throw std::runtime_error("MMI: integrity check failed");
        uint8_t diff = 0;
        for (size_t i = 0; i < tag.size(); ++i)
            diff |= (uint8_t)(expect[i] ^ tag[i]);
        if (diff != 0)
            throw std::runtime_error("MMI: integrity check failed (tampered)");

        return chacha20_xor(key.data(), nonce.data(), ciphertext);
    }
}