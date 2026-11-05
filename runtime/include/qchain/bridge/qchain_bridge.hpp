#pragma once
//
// qchain :: 桥接层 —— 面向 rvdl 语言 / C 的 ABI 门面
//
// 将 QChainService 以 C 链接导出为 rvdl_qchain_* 系列函数，使 rvdl 语言能直接编写
// 量子币种与量子区块链服务。遵循 qbNSBridge.hpp 的既有模式：
//   • 复用 runtime 探测到的全局后端 global_qm（与 rvdl 的 alloc/measure 共享后端）；
//   • 返回字符串写入 global_string_buffer；
//   • 函数体置于 #if defined(RVDL_RT_BUILD) 块内，编译 rvdl_rt.dll 时链接；
//   • 符号经 qhal/JIT.hpp 的 HostApiMap 注册，供 ORC JIT 解析。
//
#include "../services/qchain_service.hpp"
#include "../primitives/hash.hpp"
#include "../primitives/sha3.hpp"
#include "../../utils/UnicodeHash256.hpp"
#include "../../qml/Inference.hpp" // 提供 ::QObject / global_qm / global_string_buffer / RVDL_HOST_EXPORT
#include <cstdint>
#include <cstring>
#include <vector>

namespace qchain_bridge
{
    // 全局单例服务：复用 runtime 的 global_qm 后端。
    inline qchain::QChainService &service()
    {
        static qchain::QChainService s(global_qm, qchain::QChainService::Config{});
        return s;
    }

    // 十六进制地址字符串 → 量子安全地址（Hash256）。
    inline qchain::ledger::Address parse_address(const char *hex)
    {
        qchain::Bytes b = qchain::from_hex(hex ? hex : "");
        qchain::ledger::Address a{};
        size_t n = b.size() < a.size() ? b.size() : a.size();
        for (size_t i = 0; i < n; ++i)
            a[i] = b[i];
        return a;
    }

    // 已铸造的量子钞票登记表（QObject.qlm_data 存索引 +1，data_kind=QOBJ_DATA_NONE）。
    inline std::vector<qchain::token::QuantumMoney::Banknote> g_coin_notes;

    // 全局后量子签名密钥对（ML-DSA-65），供 qchain_sign / qchain_sign_verify 使用。
    inline std::pair<qchain::Bytes, qchain::Bytes> &global_sig_key()
    {
        static std::pair<qchain::Bytes, qchain::Bytes> key = []()
        {
            auto [pk, sk] = qchain::pqc::MlDsa65::keygen();
            return std::make_pair(sk, pk); // (sk, pk)
        }();
        return key;
    }

    // 全局因果哨兵（快子场），供 qchain_causal_verify 使用。
    inline qchain::causality::CausalityGuard &global_causal_guard()
    {
        static qchain::causality::CausalityGuard g;
        return g;
    }

    // UTF-8 文本 → 字节（rvdl 的 string 参数）。
    inline qchain::Bytes text_bytes(const char *s)
    {
        return qchain::Bytes(s ? s : "", s ? s + std::strlen(s) : "");
    }
} // namespace qchain_bridge

extern "C"
{
    // ─── 钱包与地址 ──────────────────────────────────────────────
    RVDL_HOST_EXPORT const char *rvdl_qchain_wallet();
    RVDL_HOST_EXPORT uint64_t rvdl_qchain_balance(const char *addr);

    // ─── 铸币与转账 ──────────────────────────────────────────────
    RVDL_HOST_EXPORT void rvdl_qchain_mint(const char *addr, uint64_t amount);
    RVDL_HOST_EXPORT int32_t rvdl_qchain_transfer(const char *from, const char *to, uint64_t amount);

    // ─── 出块与验链 ──────────────────────────────────────────────
    RVDL_HOST_EXPORT int32_t rvdl_qchain_mine();
    RVDL_HOST_EXPORT int32_t rvdl_qchain_height();
    RVDL_HOST_EXPORT int32_t rvdl_qchain_verify();

    // ─── QKD 量子密钥分发 ────────────────────────────────────────
    RVDL_HOST_EXPORT const char *rvdl_qchain_qkd(int32_t rounds);

    // ─── 量子拜占庭共识 ──────────────────────────────────────────
    RVDL_HOST_EXPORT int32_t rvdl_qchain_qdba(int32_t parties);

    // ─── 量子货币（Wiesner 不可克隆钞票）────────────────────────
    RVDL_HOST_EXPORT void *rvdl_qchain_coin_mint(int32_t qubits);
    RVDL_HOST_EXPORT int32_t rvdl_qchain_coin_verify(void *coin);

    // ─── 哈希 / 语义哈希 / HMAC ──────────────────────────────────
    RVDL_HOST_EXPORT const char *rvdl_qchain_sha3(const char *msg);          // SHA3-256，返回 hex
    RVDL_HOST_EXPORT const char *rvdl_qchain_hmac(const char *key_hex, const char *msg); // HMAC-SHA256，返回 hex
    RVDL_HOST_EXPORT const char *rvdl_qchain_hash_unicode(const char *msg);  // UnicodeHash256（汉字→拼音→SHA-256），返回 hex

    // ─── 后量子签名（ML-DSA-65，全局密钥）───────────────────────
    RVDL_HOST_EXPORT const char *rvdl_qchain_sign(const char *msg);          // 签名，返回 hex
    RVDL_HOST_EXPORT int32_t rvdl_qchain_sign_verify(const char *msg, const char *sig_hex);
    RVDL_HOST_EXPORT const char *rvdl_qchain_sign_pubkey();                  // 全局公钥 hex

    // ─── 后量子 KEM（ML-KEM-768）────────────────────────────────
    RVDL_HOST_EXPORT const char *rvdl_qchain_mlkem_encaps(const char *pk_hex); // 返回 "ct:ss"（hex）
    RVDL_HOST_EXPORT const char *rvdl_qchain_mlkem_decaps(const char *sk_hex, const char *ct_hex); // 返回 ss（hex）

    // ─── 因果哨兵（防超时空）────────────────────────────────────
    RVDL_HOST_EXPORT int32_t rvdl_qchain_causal_verify();

    // ─── 时空加密（OFB 流加密）───────────────────────────────────
    RVDL_HOST_EXPORT const char *rvdl_qchain_cipher_encrypt(uint64_t seed, const char *msg); // 返回 hex
    RVDL_HOST_EXPORT const char *rvdl_qchain_cipher_decrypt(uint64_t seed, const char *ct_hex); // 返回文本
}

#if defined(RVDL_RT_BUILD)

const char *rvdl_qchain_wallet()
{
    auto &w = qchain_bridge::service().create_wallet();
    global_string_buffer = qchain::to_hex(w.address);
    return global_string_buffer.c_str();
}

uint64_t rvdl_qchain_balance(const char *addr)
{
    return qchain_bridge::service().balance_of_address(qchain_bridge::parse_address(addr));
}

void rvdl_qchain_mint(const char *addr, uint64_t amount)
{
    qchain_bridge::service().mint_to_address(qchain_bridge::parse_address(addr), amount);
}

int32_t rvdl_qchain_transfer(const char *from, const char *to, uint64_t amount)
{
    bool ok = qchain_bridge::service().transfer_addresses(
        qchain_bridge::parse_address(from), qchain_bridge::parse_address(to), amount);
    return ok ? 1 : 0;
}

int32_t rvdl_qchain_mine()
{
    // 出块失败（PoW 不满足 / 内存池交易无效 / 链层校验拒绝）返回 -1，
    // 且交易保留在内存池中，不会像旧实现那样静默丢失。
    if (!qchain_bridge::service().mine_block())
        return -1;
    return static_cast<int32_t>(qchain_bridge::service().get_chain().height());
}

int32_t rvdl_qchain_height()
{
    return static_cast<int32_t>(qchain_bridge::service().get_chain().height());
}

int32_t rvdl_qchain_verify()
{
    return qchain_bridge::service().verify_chain() ? 1 : 0;
}

const char *rvdl_qchain_qkd(int32_t rounds)
{
    auto res = qchain_bridge::service().establish_qkd(static_cast<size_t>(rounds > 0 ? rounds : 64));
    global_string_buffer = qchain::to_hex(res.shared_key);
    return global_string_buffer.c_str();
}

int32_t rvdl_qchain_qdba(int32_t parties)
{
    auto res = qchain_bridge::service().run_qdba(static_cast<size_t>(parties), {}, 20);
    return res.agreement ? 1 : 0;
}

void *rvdl_qchain_coin_mint(int32_t qubits)
{
    size_t n = qubits > 0 ? static_cast<size_t>(qubits) : 8;
    auto note = qchain_bridge::service().mint_quantum_coin(n);

    // 登记钞票描述，返回打包的运行时 QObject（hardware_ids 指向钞票量子比特）。
    size_t idx = qchain_bridge::g_coin_notes.size();
    qchain_bridge::g_coin_notes.push_back(note);

    QObject *obj = new QObject();
    for (size_t i = 0; i < note.size(); ++i)
        obj->hardware_ids.push_back(note.start_id + i);
    obj->qlm_data = reinterpret_cast<void *>(idx + 1); // +1 避免 null
    obj->data_kind = QOBJ_DATA_NONE;
    return obj;
}

int32_t rvdl_qchain_coin_verify(void *coin)
{
    QObject *obj = static_cast<QObject *>(coin);
    if (!obj || !obj->qlm_data)
        return 0;
    size_t idx = reinterpret_cast<size_t>(obj->qlm_data) - 1;
    if (idx >= qchain_bridge::g_coin_notes.size())
        return 0;
    return qchain_bridge::service().verify_quantum_coin(qchain_bridge::g_coin_notes[idx]) ? 1 : 0;
}

// ─── 哈希 / 语义哈希 / HMAC ────────────────────────────────────────────

const char *rvdl_qchain_sha3(const char *msg)
{
    qchain::Bytes h = qchain::crypto::sha3_256(qchain_bridge::text_bytes(msg));
    global_string_buffer = qchain::to_hex(h);
    return global_string_buffer.c_str();
}

const char *rvdl_qchain_hmac(const char *key_hex, const char *msg)
{
    qchain::Bytes key = qchain::from_hex(key_hex ? key_hex : "");
    qchain::Hash256 h = qchain::crypto::hmac_sha256(key, qchain_bridge::text_bytes(msg));
    global_string_buffer = qchain::to_hex(h);
    return global_string_buffer.c_str();
}

const char *rvdl_qchain_hash_unicode(const char *msg)
{
    std::array<uint8_t, 32> h = qhal::unicode_hash256(msg ? msg : "");
    global_string_buffer = qchain::to_hex(qchain::Bytes(h.begin(), h.end()));
    return global_string_buffer.c_str();
}

// ─── 后量子签名（ML-DSA-65，全局密钥）────────────────────────────────

const char *rvdl_qchain_sign(const char *msg)
{
    auto &[sk, pk] = qchain_bridge::global_sig_key();
    qchain::Bytes sig = qchain::pqc::MlDsa65::sign(sk, qchain_bridge::text_bytes(msg));
    global_string_buffer = qchain::to_hex(sig);
    return global_string_buffer.c_str();
}

int32_t rvdl_qchain_sign_verify(const char *msg, const char *sig_hex)
{
    auto &[sk, pk] = qchain_bridge::global_sig_key();
    qchain::Bytes sig = qchain::from_hex(sig_hex ? sig_hex : "");
    return qchain::pqc::MlDsa65::verify(pk, qchain_bridge::text_bytes(msg), sig) ? 1 : 0;
}

const char *rvdl_qchain_sign_pubkey()
{
    auto &[sk, pk] = qchain_bridge::global_sig_key();
    global_string_buffer = qchain::to_hex(pk);
    return global_string_buffer.c_str();
}

// ─── 后量子 KEM（ML-KEM-768）─────────────────────────────────────────

const char *rvdl_qchain_mlkem_encaps(const char *pk_hex)
{
    qchain::Bytes pk = qchain::from_hex(pk_hex ? pk_hex : "");
    auto [ct, ss] = qchain::pqc::MlKem768::encaps(pk);
    global_string_buffer = qchain::to_hex(ct) + ":" + qchain::to_hex(ss);
    return global_string_buffer.c_str();
}

const char *rvdl_qchain_mlkem_decaps(const char *sk_hex, const char *ct_hex)
{
    qchain::Bytes sk = qchain::from_hex(sk_hex ? sk_hex : "");
    qchain::Bytes ct = qchain::from_hex(ct_hex ? ct_hex : "");
    qchain::Bytes ss = qchain::pqc::MlKem768::decaps(sk, ct);
    global_string_buffer = qchain::to_hex(ss);
    return global_string_buffer.c_str();
}

// ─── 因果哨兵（防超时空）────────────────────────────────────────────

int32_t rvdl_qchain_causal_verify()
{
    auto &g = qchain_bridge::global_causal_guard();
    g.step();
    return g.verify() ? 1 : 0;
}

// ─── 时空加密（OFB 流加密）───────────────────────────────────────────

const char *rvdl_qchain_cipher_encrypt(uint64_t seed, const char *msg)
{
    qchain::spacetime_crypto::SpacetimeCipher cipher(seed, qchain::spacetime_crypto::CipherMode::OFB);
    qchain::Bytes ct = cipher.encrypt(qchain_bridge::text_bytes(msg));
    global_string_buffer = qchain::to_hex(ct);
    return global_string_buffer.c_str();
}

const char *rvdl_qchain_cipher_decrypt(uint64_t seed, const char *ct_hex)
{
    qchain::spacetime_crypto::SpacetimeCipher cipher(seed, qchain::spacetime_crypto::CipherMode::OFB);
    qchain::Bytes ct = qchain::from_hex(ct_hex ? ct_hex : "");
    qchain::Bytes pt = cipher.decrypt(ct);
    global_string_buffer.assign(pt.begin(), pt.end());
    return global_string_buffer.c_str();
}

#endif