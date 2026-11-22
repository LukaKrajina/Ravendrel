#pragma once
#include "../services/qchain_service.hpp"
#include "../primitives/hash.hpp"
#include "../primitives/sha3.hpp"
#include "../../utils/UnicodeHash256.hpp"
#include "../../qml/Inference.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

namespace qchain_bridge
{
    inline qchain::QChainService &service()
    {
        static qchain::QChainService s(global_qm, qchain::QChainService::Config{});
        return s;
    }

    inline qchain::ledger::Address parse_address(const char *hex)
    {
        qchain::Bytes b = qchain::from_hex(hex ? hex : "");
        qchain::ledger::Address a{};
        size_t n = b.size() < a.size() ? b.size() : a.size();
        for (size_t i = 0; i < n; ++i)
            a[i] = b[i];
        return a;
    }

    inline std::vector<qchain::token::QuantumMoney::Banknote> g_coin_notes;

    inline std::pair<qchain::Bytes, qchain::Bytes> &global_sig_key()
    {
        static std::pair<qchain::Bytes, qchain::Bytes> key = []()
        {
            auto [pk, sk] = qchain::pqc::MlDsa65::keygen();
            return std::make_pair(sk, pk);
        }();
        return key;
    }

    inline qchain::causality::CausalityGuard &global_causal_guard()
    {
        static qchain::causality::CausalityGuard g;
        return g;
    }

    inline qchain::Bytes text_bytes(const char *s)
    {
        return qchain::Bytes(s ? s : "", s ? s + std::strlen(s) : "");
    }
}

extern "C"
{
    RVDL_HOST_EXPORT const char *rvdl_qchain_wallet();
    RVDL_HOST_EXPORT uint64_t rvdl_qchain_balance(const char *addr);

    RVDL_HOST_EXPORT void rvdl_qchain_mint(const char *addr, uint64_t amount);
    RVDL_HOST_EXPORT int32_t rvdl_qchain_transfer(const char *from, const char *to, uint64_t amount);

    RVDL_HOST_EXPORT int32_t rvdl_qchain_mine();
    RVDL_HOST_EXPORT int32_t rvdl_qchain_height();
    RVDL_HOST_EXPORT int32_t rvdl_qchain_verify();

    RVDL_HOST_EXPORT const char *rvdl_qchain_qkd(int32_t rounds);

    RVDL_HOST_EXPORT int32_t rvdl_qchain_qdba(int32_t parties);

    RVDL_HOST_EXPORT void *rvdl_qchain_coin_mint(int32_t qubits);
    RVDL_HOST_EXPORT int32_t rvdl_qchain_coin_verify(void *coin);

    RVDL_HOST_EXPORT const char *rvdl_qchain_sha3(const char *msg);
    RVDL_HOST_EXPORT const char *rvdl_qchain_hmac(const char *key_hex, const char *msg);
    RVDL_HOST_EXPORT const char *rvdl_qchain_hash_unicode(const char *msg);

    RVDL_HOST_EXPORT const char *rvdl_qchain_sign(const char *msg);
    RVDL_HOST_EXPORT int32_t rvdl_qchain_sign_verify(const char *msg, const char *sig_hex);
    RVDL_HOST_EXPORT const char *rvdl_qchain_sign_pubkey();

    RVDL_HOST_EXPORT const char *rvdl_qchain_mlkem_encaps(const char *pk_hex);
    RVDL_HOST_EXPORT const char *rvdl_qchain_mlkem_decaps(const char *sk_hex, const char *ct_hex);

    RVDL_HOST_EXPORT int32_t rvdl_qchain_causal_verify();

    RVDL_HOST_EXPORT const char *rvdl_qchain_cipher_encrypt(uint64_t seed, const char *msg);
    RVDL_HOST_EXPORT const char *rvdl_qchain_cipher_decrypt(uint64_t seed, const char *ct_hex);
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

    size_t idx = qchain_bridge::g_coin_notes.size();
    qchain_bridge::g_coin_notes.push_back(note);

    QObject *obj = new QObject();
    for (size_t i = 0; i < note.size(); ++i)
        obj->hardware_ids.push_back(note.start_id + i);
    obj->qlm_data = reinterpret_cast<void *>(idx + 1);
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


int32_t rvdl_qchain_causal_verify()
{
    auto &g = qchain_bridge::global_causal_guard();
    g.step();
    return g.verify() ? 1 : 0;
}


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