#pragma once
#include "../common.hpp"
#include "hash.hpp"
#include "ml_kem.hpp"
#include "ml_dsa.hpp"
#include "uov.hpp"
#include <memory>
#include <utility>
#include <stdexcept>

namespace qchain::pqc
{

    using qchain::Byte;
    using qchain::Bytes;
    using qchain::Hash256;
    using qchain::Rng;
    using qchain::Csprng;
    using crypto::sha256;
    using crypto::MerkleTree;

    enum class KemScheme
    {
        ML_KEM_512,
        ML_KEM_768,
        ML_KEM_1024,
        LWE_REFERENCE
    };

    enum class SigScheme
    {
        ML_DSA,
        SLH_DSA,
        FN_DSA,
        UOV,
        LAMPORT_OTS,
        MERKLE_XMSS
    };

    class IKem
    {
    public:
        virtual ~IKem() = default;
        virtual std::pair<Bytes, Bytes> keygen() = 0;
        virtual std::pair<Bytes, Bytes> encaps(const Bytes &public_key) = 0;
        virtual Bytes decaps(const Bytes &secret_key, const Bytes &ciphertext) = 0;
    };

    class ISignatureScheme
    {
    public:
        virtual ~ISignatureScheme() = default;
        virtual std::pair<Bytes, Bytes> keygen() = 0;
        virtual Bytes sign(const Bytes &secret_key, const Bytes &message) const = 0;
        virtual bool verify(const Bytes &public_key, const Bytes &message,
                            const Bytes &signature) const = 0;
    };

    struct LweParams
    {
        static constexpr size_t n = 256;
        static constexpr int q = 4096;
        static constexpr int half = q / 2;
        static constexpr int quarter = q / 4;
    };

    inline int modq(int64_t x)
    {
        int64_t r = x % LweParams::q;
        if (r < 0)
            r += LweParams::q;
        return static_cast<int>(r);
    }

    inline Bytes serialize_u16(const std::vector<int> &v)
    {
        Bytes b;
        b.reserve(v.size() * 2);
        for (int x : v)
        {
            uint16_t u = static_cast<uint16_t>(modq(x));
            b.push_back(Byte(u & 0xFF));
            b.push_back(Byte(u >> 8));
        }
        return b;
    }

    inline std::vector<int> deserialize_u16(const Bytes &b, size_t n)
    {
        if (b.size() < n * 2)
            throw std::invalid_argument("qchain::pqc::deserialize_u16: bad length");
        std::vector<int> v(n);
        for (size_t i = 0; i < n; ++i)
            v[i] = int(uint16_t(b[2 * i] | (b[2 * i + 1] << 8)));
        return v;
    }

    class LweKem : public IKem
    {
    public:
        struct PublicKey
        {
            std::vector<int> A;
            std::vector<int> t;
        };
        struct SecretKey
        {
            std::vector<int> s;
        };
        struct Ciphertext
        {
            std::vector<int> u;
            std::vector<int> v;
        };

        static SecretKey gen_secret(Csprng &rng)
        {
            SecretKey sk;
            sk.s.resize(LweParams::n);
            for (auto &x : sk.s)
                x = rng.cbd(1);
            return sk;
        }

        static PublicKey gen_public(const SecretKey &sk, Csprng &rng)
        {
            const size_t n = LweParams::n;
            PublicKey pk;
            pk.A.resize(n * n);
            for (auto &a : pk.A)
                a = static_cast<int>(rng.uniform(LweParams::q));
            std::vector<int> e(n);
            for (auto &x : e)
                x = rng.cbd(1);
            pk.t.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                int64_t acc = e[i];
                for (size_t j = 0; j < n; ++j)
                    acc += int64_t(pk.A[i * n + j]) * sk.s[j];
                pk.t[i] = modq(acc);
            }
            return pk;
        }

        static std::pair<Ciphertext, Hash256> encaps(const PublicKey &pk, Csprng &rng)
        {
            const size_t n = LweParams::n;
            std::vector<int> m(n), r(n), e1(n), e2(n);
            for (size_t i = 0; i < n; ++i)
            {
                m[i] = static_cast<int>(rng.uniform(2));
                r[i] = rng.cbd(1);
                e1[i] = rng.cbd(1);
                e2[i] = rng.cbd(1);
            }
            Ciphertext ct;
            ct.u.resize(n);
            for (size_t j = 0; j < n; ++j)
            {
                int64_t acc = e1[j];
                for (size_t i = 0; i < n; ++i)
                    acc += int64_t(pk.A[i * n + j]) * r[i];
                ct.u[j] = modq(acc);
            }
            ct.v.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                int64_t acc = e2[i] + int64_t(m[i]) * LweParams::half;
                for (size_t j = 0; j < n; ++j)
                    acc += int64_t(pk.t[j]) * r[j];
                ct.v[i] = modq(acc);
            }
            Bytes seed;
            for (auto b : m)
                seed.push_back(Byte(b));
            Bytes ctser = serialize(ct);
            seed.insert(seed.end(), ctser.begin(), ctser.end());
            return {ct, sha256(seed)};
        }

        static Hash256 decaps(const SecretKey &sk, const Ciphertext &ct)
        {
            const size_t n = LweParams::n;
            std::vector<int> m(n);
            for (size_t i = 0; i < n; ++i)
            {
                int64_t acc = ct.v[i];
                for (size_t j = 0; j < n; ++j)
                    acc -= int64_t(sk.s[j]) * ct.u[j];
                int d = modq(acc);
                m[i] = (d >= LweParams::quarter && d < 3 * LweParams::quarter) ? 1 : 0;
            }
            Bytes seed;
            for (auto b : m)
                seed.push_back(Byte(b));
            Bytes ctser = serialize(ct);
            seed.insert(seed.end(), ctser.begin(), ctser.end());
            return sha256(seed);
        }

        static Bytes serialize(const Ciphertext &ct)
        {
            Bytes u = serialize_u16(ct.u);
            Bytes v = serialize_u16(ct.v);
            u.insert(u.end(), v.begin(), v.end());
            return u;
        }
        static Bytes serialize_pk(const PublicKey &pk)
        {
            Bytes a = serialize_u16(pk.A);
            Bytes t = serialize_u16(pk.t);
            a.insert(a.end(), t.begin(), t.end());
            return a;
        }
        static Bytes serialize_sk(const SecretKey &sk) { return serialize_u16(sk.s); }
        static PublicKey deserialize_pk(const Bytes &b)
        {
            const size_t n = LweParams::n;
            if (b.size() < 2 * n * n + 2 * n)
                throw std::invalid_argument("qchain::pqc::LweKem::deserialize_pk: bad length");
            PublicKey pk;
            pk.A = deserialize_u16(b, n * n);
            pk.t = deserialize_u16(Bytes(b.begin() + 2 * n * n, b.end()), n);
            return pk;
        }
        static SecretKey deserialize_sk(const Bytes &b)
        {
            if (b.size() < 2 * LweParams::n)
                throw std::invalid_argument("qchain::pqc::LweKem::deserialize_sk: bad length");
            SecretKey sk;
            sk.s = deserialize_u16(b, LweParams::n);
            return sk;
        }
        static Ciphertext deserialize_ct(const Bytes &b)
        {
            const size_t n = LweParams::n;
            if (b.size() < 4 * n)
                throw std::invalid_argument("qchain::pqc::LweKem::deserialize_ct: bad length");
            Ciphertext ct;
            ct.u = deserialize_u16(b, n);
            ct.v = deserialize_u16(Bytes(b.begin() + 2 * n, b.end()), n);
            return ct;
        }

        LweKem() = default;
        explicit LweKem(uint64_t seed) : csprng(seed_bytes(seed)) {}

        static Bytes seed_bytes(uint64_t seed)
        {
            Bytes b(8);
            for (int i = 0; i < 8; ++i)
                b[i] = Byte((seed >> (8 * i)) & 0xFF);
            return b;
        }

        std::pair<Bytes, Bytes> keygen() override
        {
            SecretKey sk = gen_secret(csprng);
            PublicKey pk = gen_public(sk, csprng);
            return {serialize_sk(sk), serialize_pk(pk)};
        }

        std::pair<Bytes, Bytes> encaps(const Bytes &public_key) override
        {
            PublicKey pk = deserialize_pk(public_key);
            auto [ct, ss] = encaps(pk, csprng);
            return {serialize(ct), Bytes(ss.begin(), ss.end())};
        }

        Bytes decaps(const Bytes &secret_key, const Bytes &ciphertext) override
        {
            SecretKey sk = deserialize_sk(secret_key);
            Ciphertext ct = deserialize_ct(ciphertext);
            Hash256 ss = decaps(sk, ct);
            return Bytes(ss.begin(), ss.end());
        }

    private:
        Csprng csprng;
    };

    class MlKem768Kem : public IKem
    {
    public:
        std::pair<Bytes, Bytes> keygen() override
        {
            auto [ek, dk] = MlKem768::keygen();
            return {dk, ek};
        }

        std::pair<Bytes, Bytes> encaps(const Bytes &public_key) override
        {
            return MlKem768::encaps(public_key);
        }

        Bytes decaps(const Bytes &secret_key, const Bytes &ciphertext) override
        {
            return MlKem768::decaps(secret_key, ciphertext);
        }
    };

    class MlDsa65Signature : public ISignatureScheme
    {
    public:
        std::pair<Bytes, Bytes> keygen() override
        {
            auto [pk, sk] = MlDsa65::keygen();
            return {sk, pk};
        }

        Bytes sign(const Bytes &secret_key, const Bytes &message) const override
        {
            return MlDsa65::sign(secret_key, message);
        }

        bool verify(const Bytes &public_key, const Bytes &message,
                    const Bytes &signature) const override
        {
            return MlDsa65::verify(public_key, message, signature);
        }
    };

    class UovSignatureScheme : public ISignatureScheme
    {
    public:
        std::pair<Bytes, Bytes> keygen() override
        {
            auto [sk, pk] = UovSignature::keygen();
            return {sk, pk};
        }

        Bytes sign(const Bytes &secret_key, const Bytes &message) const override
        {
            return UovSignature::sign(secret_key, message);
        }

        bool verify(const Bytes &public_key, const Bytes &message,
                    const Bytes &signature) const override
        {
            return UovSignature::verify(public_key, message, signature);
        }
    };

    class LamportOts : public ISignatureScheme
    {
    public:
        std::pair<Bytes, Bytes> keygen() override
        {
            Csprng csprng;
            Bytes sk = csprng.random_bytes(256 * 2 * 32);
            Bytes pk(256 * 2 * 32);
            for (int i = 0; i < 256; ++i)
            {
                Hash256 h0 = sha256(sk.data() + (i * 2) * 32, 32);
                Hash256 h1 = sha256(sk.data() + (i * 2 + 1) * 32, 32);
                std::copy(h0.begin(), h0.end(), pk.begin() + (i * 2) * 32);
                std::copy(h1.begin(), h1.end(), pk.begin() + (i * 2 + 1) * 32);
            }
            return {sk, pk};
        }

        Bytes sign(const Bytes &secret_key, const Bytes &message) const override
        {
            if (secret_key.size() != 256 * 2 * 32)
                throw std::invalid_argument("qchain::LamportOts::sign: bad sk size");
            Hash256 h = sha256(message);
            Bytes sig(256 * 32);
            for (int i = 0; i < 256; ++i)
            {
                int bit = (h[i / 8] >> (7 - i % 8)) & 1;
                const Byte *src = secret_key.data() + (i * 2 + bit) * 32;
                std::copy(src, src + 32, sig.begin() + i * 32);
            }
            return sig;
        }

        bool verify(const Bytes &public_key, const Bytes &message, const Bytes &sig) const override
        {
            if (public_key.size() != 256 * 2 * 32 || sig.size() != 256 * 32)
                return false;
            Hash256 h = sha256(message);
            for (int i = 0; i < 256; ++i)
            {
                int bit = (h[i / 8] >> (7 - i % 8)) & 1;
                Hash256 hh = sha256(sig.data() + i * 32, 32);
                const Byte *expected = public_key.data() + (i * 2 + bit) * 32;
                if (!std::equal(expected, expected + 32, hh.begin()))
                    return false;
            }
            return true;
        }
    };

    class MerkleSignatureScheme
    {
    private:
        size_t height;
        std::vector<Bytes> leaves_sk;
        std::vector<Bytes> leaves_pk;
        Hash256 root{};
        size_t next_index = 0;

    public:
        explicit MerkleSignatureScheme(size_t tree_height = 4) : height(tree_height) {}

        Bytes keygen()
        {
            const size_t count = size_t(1) << height;
            leaves_sk.clear();
            leaves_pk.clear();
            std::vector<Hash256> leaf_hashes;
            leaf_hashes.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                LamportOts ots;
                auto [sk, pk] = ots.keygen();
                leaves_sk.push_back(std::move(sk));
                leaves_pk.push_back(std::move(pk));
                leaf_hashes.push_back(sha256(leaves_pk.back()));
            }
            MerkleTree tree(leaf_hashes);
            root = tree.root();
            next_index = 0;
            Bytes pk;
            pk.push_back(Byte(height & 0xFF));
            pk.insert(pk.end(), root.begin(), root.end());
            return pk;
        }

        Bytes sign(const Bytes &message)
        {
            if (next_index >= leaves_sk.size())
                throw std::runtime_error("qchain::MerkleSignatureScheme: keys exhausted");
            size_t idx = next_index++;
            LamportOts ots;
            Bytes lamsig = ots.sign(leaves_sk[idx], message);

            std::vector<Hash256> leaf_hashes;
            leaf_hashes.reserve(leaves_pk.size());
            for (auto &pk : leaves_pk)
                leaf_hashes.push_back(sha256(pk));
            MerkleTree tree(leaf_hashes);
            std::vector<Hash256> path = tree.proof(idx);

            Bytes sig;
            for (int i = 0; i < 8; ++i)
                sig.push_back(Byte((idx >> (8 * i)) & 0xFF));
            sig.insert(sig.end(), lamsig.begin(), lamsig.end());
            sig.insert(sig.end(), leaves_pk[idx].begin(), leaves_pk[idx].end());
            for (auto &sib : path)
                sig.insert(sig.end(), sib.begin(), sib.end());
            return sig;
        }

        static bool verify(const Bytes &public_key, const Bytes &message, const Bytes &sig)
        {
            const size_t lam = 256 * 32;
            const size_t pk = 256 * 2 * 32;
            if (public_key.size() != 33)
                return false;
            const size_t h = public_key[0];
            const size_t path_bytes = h * 32;
            if (sig.size() < 8 + lam + pk + path_bytes)
                return false;

            size_t idx = 0;
            for (int i = 0; i < 8; ++i)
                idx |= size_t(sig[i]) << (8 * i);

            Bytes lamsig(sig.begin() + 8, sig.begin() + 8 + lam);
            Bytes leaf_pk(sig.begin() + 8 + lam, sig.begin() + 8 + lam + pk);

            LamportOts ots;
            if (!ots.verify(leaf_pk, message, lamsig))
                return false;

            std::vector<Hash256> path;
            size_t off = 8 + lam + pk;
            for (size_t i = 0; i < h; ++i)
            {
                Hash256 sib;
                std::copy(sig.begin() + off + i * 32, sig.begin() + off + (i + 1) * 32, sib.begin());
                path.push_back(sib);
            }

            Hash256 leaf_hash = sha256(leaf_pk);
            Hash256 computed = MerkleTree::root_from_proof(leaf_hash, idx, path);
            Hash256 root;
            std::copy(public_key.begin() + 1, public_key.end(), root.begin());
            return std::equal(computed.begin(), computed.end(), root.begin());
        }

        size_t remaining() const { return leaves_sk.size() - next_index; }
    };

    inline std::unique_ptr<IKem> make_kem_scheme(KemScheme s)
    {
        switch (s)
        {
        case KemScheme::ML_KEM_768:
            return std::make_unique<MlKem768Kem>();
        case KemScheme::LWE_REFERENCE:
#if defined(RVDL_ALLOW_REFERENCE_CRYPTO)
            return std::make_unique<LweKem>();
#else
            throw std::runtime_error(
                "qchain::pqc::make_kem_scheme: LWE_REFERENCE is a demonstration-scale "
                "Regev LWE (n=256, q=4096) with no claimed security level. Define "
                "RVDL_ALLOW_REFERENCE_CRYPTO to opt in for research use only.");
#endif
        case KemScheme::ML_KEM_512:
        case KemScheme::ML_KEM_1024:
        default:
            throw std::runtime_error(
                "qchain::pqc::make_kem_scheme: requested ML-KEM parameter set is not "
                "implemented in this build (only ML-KEM-768 / FIPS 203 is available).");
        }
    }

    inline std::unique_ptr<ISignatureScheme> make_sig_scheme(SigScheme s)
    {
        switch (s)
        {
        case SigScheme::ML_DSA:
            return std::make_unique<MlDsa65Signature>();
        case SigScheme::UOV:
            return std::make_unique<UovSignatureScheme>();
        case SigScheme::LAMPORT_OTS:
            return std::make_unique<LamportOts>();
        case SigScheme::SLH_DSA:
        case SigScheme::FN_DSA:
            throw std::runtime_error(
                "qchain::pqc::make_sig_scheme: SLH-DSA (FIPS 205) / FN-DSA (FIPS 206) "
                "are reserved in the enum but not implemented in this build.");
        case SigScheme::MERKLE_XMSS:
            throw std::runtime_error(
                "qchain::pqc::make_sig_scheme: MERKLE_XMSS is a stateful scheme and is "
                "incompatible with the stateless ISignatureScheme interface; use "
                "MerkleSignatureScheme directly with explicit state management.");
        default:
            throw std::runtime_error("qchain::pqc::make_sig_scheme: unknown scheme");
        }
    }

}