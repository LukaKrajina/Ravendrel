#pragma once
#include <cstdint>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace rvdlrsp::editor
{

    class Ed25519
    {
    public:
        static constexpr size_t kPubKeySize = 32;
        static constexpr size_t kPrivKeySize = 32;
        static constexpr size_t kSigSize = 64;

        static bool verify(const uint8_t *msg, size_t msglen,
                           const uint8_t *sig, size_t siglen,
                           const uint8_t pubkey[kPubKeySize])
        {
            EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pubkey, kPubKeySize);
            if (!pkey)
                return false;
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            if (!ctx)
            {
                EVP_PKEY_free(pkey);
                return false;
            }
            int ok = (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) &&
                     (EVP_DigestVerify(ctx, sig, siglen, msg, msglen) == 1);
            EVP_MD_CTX_free(ctx);
            EVP_PKEY_free(pkey);
            return ok;
        }

        static EVP_PKEY *load_public_pem(const char *path)
        {
            FILE *f = fopen(path, "rb");
            if (!f)
                return nullptr;
            EVP_PKEY *pkey = PEM_read_PUBKEY(f, nullptr, nullptr, nullptr);
            fclose(f);
            return pkey;
        }

        static EVP_PKEY *load_private_pem(const char *path)
        {
            FILE *f = fopen(path, "rb");
            if (!f)
                return nullptr;
            EVP_PKEY *pkey = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
            fclose(f);
            return pkey;
        }

        static bool sign(const uint8_t *msg, size_t msglen, EVP_PKEY *privkey,
                         uint8_t sig[kSigSize])
        {
            if (!privkey)
                return false;
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            if (!ctx)
                return false;
            size_t siglen = kSigSize;
            int ok = (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, privkey) == 1) &&
                     (EVP_DigestSign(ctx, sig, &siglen, msg, msglen) == 1);
            EVP_MD_CTX_free(ctx);
            return ok && siglen == kSigSize;
        }
    };
}