#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#ifdef small
#undef small
#endif

namespace qchain
{

    using Byte = uint8_t;
    using Bytes = std::vector<Byte>;
    using Hash256 = std::array<Byte, 32>;

    struct Hash256Hasher
    {
        size_t operator()(const Hash256 &h) const noexcept
        {
            size_t hash = 14695981039346656037ULL;
            for (Byte b : h)
            {
                hash ^= b;
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    };

    class Rng
    {
    private:
        std::mt19937_64 gen;

    public:
        Rng() { std::random_device rd; gen.seed(rd()); }
        explicit Rng(uint64_t seed) { gen.seed(seed); }

        uint64_t next_u64() { return gen(); }

        uint64_t uniform(uint64_t bound)
        {
            if (bound == 0)
                return 0;
            std::uniform_int_distribution<uint64_t> d(0, bound - 1);
            return d(gen);
        }

        double uniform_real() { return std::uniform_real_distribution<double>(0.0, 1.0)(gen); }

        int small()
        {
            uint64_t u = uniform(100);
            if (u < 25)
                return -1;
            if (u < 75)
                return 0;
            return 1;
        }

        Bytes random_bytes(size_t n)
        {
            Bytes b(n);
            for (auto &x : b)
                x = Byte(uniform(256));
            return b;
        }

        bool random_bit() { return uniform(2) == 1; }
    };

    inline bool constant_time_equal(const void *a, const void *b, size_t n)
    {
        const Byte *pa = static_cast<const Byte *>(a);
        const Byte *pb = static_cast<const Byte *>(b);
        Byte diff = 0;
        for (size_t i = 0; i < n; ++i)
            diff = static_cast<Byte>(diff | (pa[i] ^ pb[i]));
        return diff == 0;
    }
    inline bool constant_time_equal(const Bytes &a, const Bytes &b)
    {
        if (a.size() != b.size())
            return false;
        return constant_time_equal(a.data(), b.data(), a.size());
    }
    inline bool constant_time_equal(const Hash256 &a, const Hash256 &b)
    {
        return constant_time_equal(a.data(), b.data(), a.size());
    }

    inline void secure_wipe(void *ptr, size_t n)
    {
        volatile Byte *p = static_cast<volatile Byte *>(ptr);
        while (n--)
            *p++ = 0;
    }
    inline void secure_wipe(Bytes &b)
    {
        if (!b.empty())
            secure_wipe(b.data(), b.size());
    }

    class Csprng
    {
    private:
        uint32_t state_[16];
        uint32_t block_[16];
        size_t block_pos_ = 16;

        static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

        static void quarter_round(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d)
        {
            a += b; d ^= a; d = rotl32(d, 16);
            c += d; b ^= c; b = rotl32(b, 12);
            a += b; d ^= a; d = rotl32(d, 8);
            c += d; b ^= c; b = rotl32(b, 7);
        }

        void init(const uint32_t key[8], const uint32_t nonce[3])
        {
            static const uint32_t constants[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
            for (int i = 0; i < 4; ++i)
                state_[i] = constants[i];
            for (int i = 0; i < 8; ++i)
                state_[4 + i] = key[i];
            state_[12] = 0;
            state_[13] = 0;
            for (int i = 0; i < 3; ++i)
                state_[14 + i] = nonce[i];
            block_pos_ = 16;
        }

        void generate_block()
        {
            uint32_t x[16];
            for (int i = 0; i < 16; ++i)
                x[i] = state_[i];
            for (int r = 0; r < 10; ++r)
            {
                quarter_round(x[0], x[4], x[8], x[12]);
                quarter_round(x[1], x[5], x[9], x[13]);
                quarter_round(x[2], x[6], x[10], x[14]);
                quarter_round(x[3], x[7], x[11], x[15]);
                quarter_round(x[0], x[5], x[10], x[15]);
                quarter_round(x[1], x[6], x[11], x[12]);
                quarter_round(x[2], x[7], x[8], x[13]);
                quarter_round(x[3], x[4], x[9], x[14]);
            }
            for (int i = 0; i < 16; ++i)
                block_[i] = x[i] + state_[i];
            if (++state_[12] == 0)
                ++state_[13];
            block_pos_ = 0;
        }

        uint32_t next_u32()
        {
            if (block_pos_ >= 16)
                generate_block();
            return block_[block_pos_++];
        }

    public:
        Csprng()
        {
            uint32_t key[8], nonce[3];
            std::random_device rd;
            for (auto &k : key)
                k = rd();
            for (auto &n : nonce)
                n = rd();
            init(key, nonce);
        }

        explicit Csprng(const Bytes &seed)
        {
            uint32_t key[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            uint32_t nonce[3] = {0, 0, 0};
            for (size_t i = 0; i < seed.size(); ++i)
            {
                size_t idx = i % 44;
                Byte b = seed[i];
                if (idx < 32)
                    key[idx / 4] |= uint32_t(b) << (8 * (idx % 4));
                else
                    nonce[(idx - 32) / 4] |= uint32_t(b) << (8 * ((idx - 32) % 4));
            }
            init(key, nonce);
        }

        Bytes random_bytes(size_t n)
        {
            Bytes b(n);
            for (size_t i = 0; i < n; ++i)
                b[i] = static_cast<Byte>(next_u32() & 0xFF);
            return b;
        }

        uint64_t next_u64()
        {
            uint64_t lo = next_u32(), hi = next_u32();
            return (hi << 32) | lo;
        }

        uint64_t uniform(uint64_t bound)
        {
            if (bound == 0)
                return 0;
            uint64_t threshold = (-bound) % bound;
            for (;;)
            {
                uint64_t r = next_u64();
                if (r >= threshold)
                    return r % bound;
            }
        }

        bool random_bit() { return (next_u32() & 1) != 0; }

        int cbd(int eta)
        {
            int x = 0;
            for (int i = 0; i < eta; ++i)
                x += (next_u32() & 1);
            for (int i = 0; i < eta; ++i)
                x -= (next_u32() & 1);
            return x;
        }
    };


    inline std::string to_hex(const Bytes &data)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (Byte b : data)
            oss << std::setw(2) << static_cast<int>(b);
        return oss.str();
    }

    inline std::string to_hex(const Hash256 &h)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (Byte b : h)
            oss << std::setw(2) << static_cast<int>(b);
        return oss.str();
    }

    inline Bytes to_bytes(const Hash256 &h) { return Bytes(h.begin(), h.end()); }

    inline Bytes from_hex(const std::string &s)
    {
        if (s.size() % 2 != 0)
            throw std::invalid_argument("qchain::from_hex: odd-length hex string");
        for (char ch : s)
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            {
                std::cerr << "[DEBUG from_hex] input(len=" << s.size() << ") = \"" << s << "\"\n";
                break;
            }
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            throw std::invalid_argument("qchain::from_hex: invalid hex char");
        };
        Bytes out;
        out.reserve(s.size() / 2);
        for (size_t i = 0; i < s.size(); i += 2)
            out.push_back(Byte((nib(s[i]) << 4) | nib(s[i + 1])));
        return out;
    }

}