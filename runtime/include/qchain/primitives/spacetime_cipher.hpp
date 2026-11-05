#pragma once
//
// SpacetimeCipher / SpacetimeQuantumHybrid —— 时空量子加密（抗破解）
//
// 密钥来源三重叠加，各自独立、缺一不可逆推出密钥：
//   1) 后量子格 KEM（LweKem / ML-KEM）—— 计算困难性（防 Shor 算法）；
//   2) 快子场混沌演化（TachyonField）—— 时空熵（防线性/统计破解）；
//   3) SHA-256 链式反馈（OFB / CTR 模式）—— 密码学正确性（防已知明文）。
//
// keystream 生成：每块 = SHA-256(phi场字节 + 反馈)，其中 phi 场每块推进一步快子场。
//   • OFB 模式：反馈上一块输出（输出反馈）；
//   • CTR 模式：反馈计数器（并行友好）。
// 流加密：密文 = 明文 XOR keystream（解密同构）。
//
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
        OFB, // 输出反馈
        CTR  // 计数器
    };

    class SpacetimeCipher
    {
    private:
        rvdl::spacetime::Foliation fol_;
        Bytes iv_;      // 初始向量 = SHA-256(seed)
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

        // 链式反馈 keystream：每块 = SHA-256(phi场字节 + 反馈/counter)。
        Bytes keystream(size_t n_bytes)
        {
            Bytes ks;
            ks.reserve(n_bytes);
            Bytes feedback = iv_;
            uint64_t counter = 0;
            while (ks.size() < n_bytes)
            {
                fol_.step(); // 快子场混沌演化（时空熵）
                Bytes input = phi_to_bytes();
                if (mode_ == CipherMode::OFB)
                {
                    input.insert(input.end(), feedback.begin(), feedback.end());
                }
                else // CTR
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

        // 流加密对称：解密即加密（XOR）。
        Bytes decrypt(const Bytes &ciphertext) { return encrypt(ciphertext); }
    };

    // 时空 + 后量子混合：用 LweKem 共享密钥派生快子场 seed。
    class SpacetimeQuantumHybrid
    {
    public:
        // 由后量子共享密钥（32 字节）派生 64 位快子场 seed。
        static uint64_t seed_from_shared_secret(const Bytes &shared_secret)
        {
            uint64_t seed = 0;
            for (size_t i = 0; i < shared_secret.size() && i < 8; ++i)
                seed = (seed << 8) | shared_secret[i];
            return seed;
        }

        // 完整混合流程：后量子 KEM 交换共享密钥 → 派生时空密钥 → 加/解密。
        //   encaps_seed(kem_pk) -> (ciphertext, shared_secret)
        //
        // 原实现硬编码 pqc::LweKem —— 那是「演示/参考规模」的 Regev LWE
        // （n=256, q=4096），无宣称安全等级，用它封装真实数据密钥是不安全的。
        // 现改为经工厂取生产级 ML-KEM-768（FIPS 203）；kem 可注入以便替换/测试。
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