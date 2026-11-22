#pragma once
#include "../common.hpp"
#include <cstring>

namespace qchain::crypto
{

    using qchain::Byte;
    using qchain::Bytes;

    namespace detail
    {
        inline uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

        inline void keccak_f1600(uint64_t st[25])
        {
            static const uint64_t rndc[24] = {
                0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
                0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
                0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
                0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
                0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
                0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
            static const uint64_t rotc[24] = {
                1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
            static const uint64_t piln[24] = {
                10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

            uint64_t bc[5], t;
            int i, j;
            for (int round = 0; round < 24; ++round)
            {
                for (i = 0; i < 5; ++i)
                    bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
                for (i = 0; i < 5; ++i)
                {
                    t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
                    for (j = 0; j < 25; j += 5)
                        st[j + i] ^= t;
                }
                t = st[1];
                for (i = 0; i < 24; ++i)
                {
                    j = static_cast<int>(piln[i]);
                    bc[0] = st[j];
                    st[j] = rotl64(t, static_cast<int>(rotc[i]));
                    t = bc[0];
                }
                for (j = 0; j < 25; j += 5)
                {
                    for (i = 0; i < 5; ++i)
                        bc[i] = st[j + i];
                    for (i = 0; i < 5; ++i)
                        st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
                }
                st[0] ^= rndc[round];
            }
        }
    }

    class KeccakSponge
    {
    private:
        uint64_t st_[25] = {0};
        size_t rate_;
        uint8_t domain_;
        size_t pos_ = 0;
        bool finalized_ = false;

    public:
        KeccakSponge(size_t rate_bytes, uint8_t domain) : rate_(rate_bytes), domain_(domain) {}

        void absorb(const uint8_t *data, size_t len)
        {
            for (size_t i = 0; i < len; ++i)
            {
                reinterpret_cast<uint8_t *>(st_)[pos_++] ^= data[i];
                if (pos_ == rate_)
                {
                    detail::keccak_f1600(st_);
                    pos_ = 0;
                }
            }
        }
        void absorb(const Bytes &data) { absorb(data.data(), data.size()); }

        void finalize()
        {
            if (finalized_)
                return;
            reinterpret_cast<uint8_t *>(st_)[pos_] ^= domain_;
            reinterpret_cast<uint8_t *>(st_)[rate_ - 1] ^= 0x80;
            detail::keccak_f1600(st_);
            pos_ = 0;
            finalized_ = true;
        }

        void squeeze(uint8_t *out, size_t len)
        {
            finalize();
            while (len > 0)
            {
                size_t take = len < rate_ ? len : rate_;
                std::memcpy(out, st_, take);
                out += take;
                len -= take;
                if (len > 0)
                    detail::keccak_f1600(st_);
            }
        }
    };

    inline Bytes sha3_256(const Bytes &data)
    {
        KeccakSponge s(136, 0x06);
        s.absorb(data);
        Bytes out(32);
        s.squeeze(out.data(), 32);
        return out;
    }

    inline Bytes sha3_512(const Bytes &data)
    {
        KeccakSponge s(72, 0x06);
        s.absorb(data);
        Bytes out(64);
        s.squeeze(out.data(), 64);
        return out;
    }

    inline Bytes shake128(const Bytes &data, size_t out_len)
    {
        KeccakSponge s(168, 0x1f);
        s.absorb(data);
        Bytes out(out_len);
        s.squeeze(out.data(), out_len);
        return out;
    }

    inline Bytes shake256(const Bytes &data, size_t out_len)
    {
        KeccakSponge s(136, 0x1f);
        s.absorb(data);
        Bytes out(out_len);
        s.squeeze(out.data(), out_len);
        return out;
    }

}