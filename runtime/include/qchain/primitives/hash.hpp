#pragma once
//
// 量子安全哈希原语（SHA-256 + Merkle 树 + 地址派生）
//
// SHA-256 是量子区块链中「抗量子」地基之一：Grover 算法对哈希碰撞仅提供平方级
// 加速（2^256 → 2^128），所以 256 位哈希在量子计算模型下仍具备充分安全性。
//
// 提供：
//   • Sha256 / sha256()   —— 标准 SHA-256
//   • concat_hash()       —— 双哈希拼接（Merkle 节点）
//   • MerkleTree          —— 交易默克尔树 + 证明生成/校验（SPV 轻节点基础）
//   • derive_address()    —— 由（后量子）公钥派生出量子安全地址
//
#include "../common.hpp"
#include <cstring>

namespace qchain::crypto
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;

    // ─── SHA-256（FIPS 180-4）──────────────────────────────────
    class Sha256
    {
    private:
        static constexpr std::array<uint32_t, 64> K = {
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

        std::array<uint32_t, 8> h;
        std::array<Byte, 64> buffer;
        size_t buffer_len = 0;
        uint64_t total_len = 0;

        static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

        void process_block(const Byte *p)
        {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                       (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
            for (int i = 16; i < 64; ++i)
            {
                uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i)
            {
                uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ (~e & g);
                uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = S0 + maj;
                hh = g; g = f; f = e; e = d + temp1;
                d = c; c = b; b = a; a = temp1 + temp2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

    public:
        Sha256()
            : h({0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
                 0x1f83d9ab, 0x5be0cd19}) {}

        void update(const Byte *data, size_t len)
        {
            total_len += len;
            size_t idx = 0;
            while (len > 0)
            {
                size_t space = 64 - buffer_len;
                size_t take = (len < space) ? len : space;
                std::memcpy(buffer.data() + buffer_len, data + idx, take);
                buffer_len += take;
                idx += take;
                len -= take;
                if (buffer_len == 64)
                {
                    process_block(buffer.data());
                    buffer_len = 0;
                }
            }
        }

        void update(const Bytes &d) { update(d.data(), d.size()); }

        Hash256 final()
        {
            uint64_t bit_len = total_len * 8;
            Byte pad = 0x80;
            update(&pad, 1);
            Byte zero = 0;
            while (buffer_len != 56)
                update(&zero, 1);
            Byte lenbuf[8];
            for (int i = 0; i < 8; ++i)
                lenbuf[i] = Byte((bit_len >> (56 - 8 * i)) & 0xFF);
            update(lenbuf, 8);
            Hash256 out;
            for (int i = 0; i < 8; ++i)
            {
                out[i * 4]     = Byte(h[i] >> 24);
                out[i * 4 + 1] = Byte(h[i] >> 16);
                out[i * 4 + 2] = Byte(h[i] >> 8);
                out[i * 4 + 3] = Byte(h[i]);
            }
            return out;
        }
    };

    inline Hash256 sha256(const void *data, size_t len)
    {
        Sha256 s;
        s.update(static_cast<const Byte *>(data), len);
        return s.final();
    }

    inline Hash256 sha256(const Bytes &data) { return sha256(data.data(), data.size()); }

    inline Hash256 concat_hash(const Hash256 &a, const Hash256 &b)
    {
        Bytes buf;
        buf.reserve(64);
        buf.insert(buf.end(), a.begin(), a.end());
        buf.insert(buf.end(), b.begin(), b.end());
        return sha256(buf);
    }

    // ─── Merkle 树（交易哈希树）────────────────────────────────────────
    class MerkleTree
    {
    private:
        std::vector<std::vector<Hash256>> levels;

    public:
        explicit MerkleTree(const std::vector<Hash256> &leaves)
        {
            if (leaves.empty())
                throw std::invalid_argument("qchain::MerkleTree: empty leaf set");
            levels.push_back(leaves);
            while (levels.back().size() > 1)
            {
                const auto &cur = levels.back();
                std::vector<Hash256> next;
                next.reserve((cur.size() + 1) / 2);
                for (size_t i = 0; i < cur.size(); i += 2)
                {
                    if (i + 1 < cur.size())
                        next.push_back(concat_hash(cur[i], cur[i + 1]));
                    else
                        next.push_back(concat_hash(cur[i], cur[i])); // 奇数叶复制
                }
                levels.push_back(std::move(next));
            }
        }

        Hash256 root() const { return levels.back()[0]; }
        size_t height() const { return levels.size(); }

        // 生成叶索引 leaf_index 的包含证明（自下而上的兄弟哈希序列）。
        std::vector<Hash256> proof(size_t leaf_index) const
        {
            if (leaf_index >= levels[0].size())
                throw std::out_of_range("qchain::MerkleTree::proof: index out of range");
            std::vector<Hash256> path;
            size_t idx = leaf_index;
            for (size_t lvl = 0; lvl + 1 < levels.size(); ++lvl)
            {
                const auto &cur = levels[lvl];
                size_t sibling = (idx ^ 1);
                path.push_back(cur[sibling]);
                idx >>= 1;
            }
            return path;
        }

        // 由「叶哈希 + 索引 + 证明」重建根（静态，供轻节点校验）。
        static Hash256 root_from_proof(const Hash256 &leaf, size_t index,
                                       const std::vector<Hash256> &path)
        {
            Hash256 cur = leaf;
            size_t idx = index;
            for (const auto &sibling : path)
            {
                if (idx & 1)
                    cur = concat_hash(sibling, cur);
                else
                    cur = concat_hash(cur, sibling);
                idx >>= 1;
            }
            return cur;
        }
    };

    // ─── 量子安全地址派生 ─────────────────────────────────────────────
    //
    // 量子区块链地址直接由（后量子）公钥哈希而来；
    // 由于使用的是 ML-DSA / SLH-DSA 等
    // 抗量子签名算法公钥，地址天然抵抗 Shor 算法对 ECDSA 公钥的逆推攻击。
    inline Hash256 derive_address(const Bytes &public_key) { return sha256(public_key); }

    // ─── HMAC-SHA256（RFC 2104）───────────────────────────────────────
    //
    // 消息认证码：H((K' ⊕ opad) ‖ H((K' ⊕ ipad) ‖ message))，K' 为密钥补零到
    // 64 字节（块长），若密钥长于 64 字节则先哈希。用于链上消息认证与 HKDF 抽取。
    inline Hash256 hmac_sha256(const Bytes &key, const Bytes &message)
    {
        constexpr size_t BLOCK = 64;
        Bytes k = key;
        if (k.size() > BLOCK)
        {
            Hash256 h = sha256(k);
            k.assign(h.begin(), h.end());
        }
        k.resize(BLOCK, 0);

        Bytes inner(BLOCK + message.size());
        for (size_t i = 0; i < BLOCK; ++i)
            inner[i] = Byte(k[i] ^ 0x36);
        std::copy(message.begin(), message.end(), inner.begin() + BLOCK);
        Hash256 inner_hash = sha256(inner);

        Bytes outer(BLOCK + 32);
        for (size_t i = 0; i < BLOCK; ++i)
            outer[i] = Byte(k[i] ^ 0x5c);
        std::copy(inner_hash.begin(), inner_hash.end(), outer.begin() + BLOCK);
        return sha256(outer);
    }

    // ─── HKDF-SHA256（RFC 5869）───────────────────────────────────────
    //
    // 密钥派生函数：从「后量子 KEM 共享密钥」等输入密钥材料（IKM）派生出任意长度
    // 的会话密钥。extract（可选 salt）+ expand（info 域分隔），一步到位。
    inline Bytes hkdf_sha256(const Bytes &ikm, const Bytes &salt, const Bytes &info,
                             size_t out_len)
    {
        // extract：PRK = HMAC(salt, IKM)；salt 为空时用 32 字节全零。
        Bytes actual_salt = salt.empty() ? Bytes(32, 0) : salt;
        Hash256 prk = hmac_sha256(actual_salt, ikm);

        Bytes okm;
        okm.reserve(out_len);
        Bytes prev; // T(i-1)，初始为空
        uint8_t counter = 1;
        while (okm.size() < out_len)
        {
            Bytes block = prev;
            block.insert(block.end(), info.begin(), info.end());
            block.push_back(counter++);
            Hash256 t = hmac_sha256(Bytes(prk.begin(), prk.end()), block);
            okm.insert(okm.end(), t.begin(), t.end());
            prev.assign(t.begin(), t.end());
        }
        okm.resize(out_len);
        return okm;
    }
}