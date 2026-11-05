#pragma once
// ─── Ed25519 签名/验证（基于 OpenSSL EVP，加载方用公钥验证）────────
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

        // 用 32 字节公钥验证签名
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

        // 从 PEM 文件加载公钥（用于打包工具/测试）
        static EVP_PKEY *load_public_pem(const char *path)
        {
            FILE *f = fopen(path, "rb");
            if (!f)
                return nullptr;
            EVP_PKEY *pkey = PEM_read_PUBKEY(f, nullptr, nullptr, nullptr);
            fclose(f);
            return pkey;
        }

        // 从 PEM 文件加载私钥（用于打包签名）
        static EVP_PKEY *load_private_pem(const char *path)
        {
            FILE *f = fopen(path, "rb");
            if (!f)
                return nullptr;
            EVP_PKEY *pkey = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
            fclose(f);
            return pkey;
        }

        // 用私钥签名（返回 64 字节签名）
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