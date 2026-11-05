#pragma once
//
// ═══════════════════════════════════════════════════════════════════════
//  qchain —— 量子加密 + 量子区块链底层架构与服务（统一入口）
// ═══════════════════════════════════════════════════════════════════════
//
//  目标：为其他开发者提供一套可复用的底层，用于创造「各种各样的量子币种与
//        量子区块链服务」。整体对齐两大研究主线：
//
//    A. 后量子区块链（Post-Quantum Blockchain）
//       —— 用 NIST 标准化/前沿后量子密码替换易受 Shor 攻击的 RSA / ECDSA：
//         · FIPS 203 ML-KEM（Kyber）      密钥封装
//         · FIPS 204 ML-DSA（Dilithium）  格签名
//         · FIPS 205 SLH-DSA（SPHINCS+）  哈希签名
//         · FIPS 206 FN-DSA（Falcon）     格签名
//         · UOV                         多变量签名（MQ 问题，27 年验证）
//         · Lamport / Merkle(XMSS)      哈希签名（无条件安全）
//       —— 参考：Quantum Blockchain Survey (arXiv:2507.13720)、
//                QuantumShield-BC (Sci. Rep. 2025)、PQ-PoETChain (Sci. Rep. 2025)。
//
//    B. 量子原生区块链（Quantum-Native Blockchain）
//       —— 以量子资源（纠缠 / QKD / 量子不可克隆）为安全地基：
//         · QKD：BB84 / E91 / 诱骗态 BB84
//         · QDS：Gottesman–Chuang 量子数字签名（arXiv:quant-ph/0105032）
//         · QDBA：GHZ 态可检测拜占庭一致（Weng et al. 2023）
//         · 量子货币：Wiesner 不可克隆货币
//
//    C. 抗超时空与抗破解（时空防线）
//       —— 用 spacetime 的物理不变量守卫因果律，用语义/时空加密对抗破解：
//         · CausalityGuard：快子场能量守恒 + 额外维 KK 对称（防超时空篡改）
//         · UnicodeHash256：汉字/符号 → 拼音/英文 → SHA-256（语义哈希）
//         · SpacetimeCipher：快子场混沌 keystream + SHA-256 OFB/CTR（时空加密）
//
//  分层架构：
//     ┌───────────────────────────────────────────────┐
//     │ services/   QChainService 服务编排（门面）      │
//     │ bridge/     C ABI 门面（rvdl / 其他语言）        │
//     ├───────────────────────────────────────────────┤
//     │ ledger/     交易 · 区块 · 链（UTXO + 哈希/纠缠/因果链）│
//     │ consensus/  量子 PoW/PoS + QDBA 最终性         │
//     │ token/      量子货币 · 可编程量子代币          │
//     ├───────────────────────────────────────────────┤
//     │ primitives/ pqc · qkd · qds · hash · causality · spacetime_cipher │
//     └───────────────────────────────────────────────┘
//     （量子后端：qhal::IQuantumBackend —— QVM 仿真 / QM 真实量子机）
//
//  快速上手：
//     #include "qchain/qchain.hpp"
//     qchain::QChainService svc(qchain::QChainService::Config{});
//     auto &alice = svc.create_wallet();
//     svc.mint(alice, 1000);
//     svc.mine_block();
//     svc.verify_chain();
//
#include "common.hpp"
#include "primitives/hash.hpp"
#include "primitives/pqc.hpp"
#include "primitives/qkd.hpp"
#include "primitives/qds.hpp"
#include "primitives/causality.hpp"
#include "primitives/spacetime_cipher.hpp"
#include "ledger/transaction.hpp"
#include "ledger/block.hpp"
#include "ledger/chain.hpp"
#include "consensus/qba.hpp"
#include "consensus/consensus.hpp"
#include "token/quantum_coin.hpp"
#include "services/qchain_service.hpp"
#include "bridge/qchain_bridge.hpp"
