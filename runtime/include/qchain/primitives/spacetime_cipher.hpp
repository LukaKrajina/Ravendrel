#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "../../spacetime/Foliation.hpp"
#include "../../spacetime/InitialConditions.hpp"
#include "../common.hpp"
#include "hash.hpp"
#include "pqc.hpp"

namespace qchain::spacetime_crypto
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using qchain::crypto::sha256;

    enum class CipherMode
    {
        OFB,
        CTR
    };

    class SpacetimeCipher
    {
    private:
        rvdl::spacetime::Foliation fol_;
        Bytes iv_;
        CipherMode mode_;

        static rvdl::spacetime::TachyonConfig make_cfg(uint64_t seed)
        {
            rvdl::spacetime::TachyonConfig c;
            c.mu2 = 1.0;
            c.lambda = 1.0;
            c.dim = 1;
            c.n = {128, 1, 1};
            c.length = {20.0, 1.0, 1.0};
            c.dt = 0.005;
            c.integrator = rvdl::spacetime::IntegratorKind::ForestRuth4;
            c.diff_order = 4;
            c.seed = seed;
            c.noise_amplitude = 1e-3;
            return c;
        }

        Bytes phi_to_bytes() const
        {
            const auto &phi = fol_.field().phi;
            Bytes b(phi.size() * sizeof(double));
            std::memcpy(b.data(), phi.data(), b.size());
            return b;
        }

    public:
        SpacetimeCipher(uint64_t seed, CipherMode mode = CipherMode::OFB)
            : fol_(make_cfg(seed)), mode_(mode)
        {
            rvdl::spacetime::init_gaussian_noise(fol_.field(), 1e-3);
            Hash256 iv = sha256(&seed, sizeof(seed));
            iv_.assign(iv.begin(), iv.end());
        }

        Bytes keystream(size_t n_bytes)
        {
            Bytes ks;
            ks.reserve(n_bytes);
            Bytes feedback = iv_;
            uint64_t counter = 0;
            while (ks.size() < n_bytes)
            {
                fol_.step();
                Bytes input = phi_to_bytes();
                if (mode_ == CipherMode::OFB)
                {
                    input.insert(input.end(), feedback.begin(), feedback.end());
                }
                else
                {
                    for (int i = 0; i < 8; ++i)
                        input.push_back(static_cast<Byte>((counter >> (8 * i)) & 0xFF));
                    ++counter;
                }
                Hash256 block = sha256(input);
                feedback.assign(block.begin(), block.end());
                ks.insert(ks.end(), block.begin(), block.end());
            }
            ks.resize(n_bytes);
            return ks;
        }

        Bytes encrypt(const Bytes &plaintext)
        {
            Bytes ks = keystream(plaintext.size());
            Bytes ct(plaintext.size());
            for (size_t i = 0; i < plaintext.size(); ++i)
                ct[i] = plaintext[i] ^ ks[i];
            return ct;
        }

        Bytes decrypt(const Bytes &ciphertext) { return encrypt(ciphertext); }
    };

    class SpacetimeQuantumHybrid
    {
    public:
        static uint64_t seed_from_shared_secret(const Bytes &shared_secret)
        {
            uint64_t seed = 0;
            for (size_t i = 0; i < shared_secret.size() && i < 8; ++i)
                seed = (seed << 8) | shared_secret[i];
            return seed;
        }

        static std::pair<Bytes, Bytes> encaps_seed(const Bytes &kem_public_key,
                                                   pqc::IKem *kem = nullptr)
        {
            std::unique_ptr<pqc::IKem> owned;
            if (!kem)
            {
                owned = pqc::make_kem_scheme(pqc::KemScheme::ML_KEM_768);
                kem = owned.get();
            }
            auto [ct, ss] = kem->encaps(kem_public_key);
            return {ct, ss};
        }
    };

}