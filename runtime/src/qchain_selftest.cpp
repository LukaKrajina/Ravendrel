#include <cassert>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "qchain/primitives/hash.hpp"
#include "qchain/primitives/sha3.hpp"
#include "qchain/primitives/pqc.hpp"
#include "qchain/primitives/ml_kem_kat.hpp"
#include "qchain/primitives/ml_dsa.hpp"
#include "qchain/primitives/ml_dsa_kat.hpp"
#include "qchain/primitives/causality.hpp"
#include "qchain/primitives/spacetime_cipher.hpp"
#include "qchain/primitives/qds.hpp"
#include "qchain/primitives/qkd.hpp"
#include "qchain/token/quantum_coin.hpp"
#include "qchain/ledger/transaction.hpp"
#include "qchain/ledger/block.hpp"
#include "qchain/ledger/chain.hpp"
#include "qhal/IQuantumBackend.hpp"
#include "utils/UnicodeHash256.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            ++g_failures;                                                      \
            std::cerr << "FAIL: " << #cond << " (line " << __LINE__ << ")\n";  \
        }                                                                      \
    } while (0)

using qchain::Bytes;
using qchain::to_hex;

class StubBackend : public qhal::IQuantumBackend
{
public:
    struct Qubit
    {
        bool x_basis = false;
        int value = 0;
        bool allocated = false;
    };

private:
    std::vector<Qubit> qubits_;
    qchain::Rng rng_;

public:
    void allocate_qubits(size_t num_qubits) override
    {
        if (num_qubits <= qubits_.size())
            return;
        const size_t old = qubits_.size();
        qubits_.resize(num_qubits);
        for (size_t i = old; i < num_qubits; ++i)
            qubits_[i] = Qubit{false, 0, true};
    }

    void release_qubit(size_t qubit_id) override
    {
        if (qubit_id >= qubits_.size())
            return;
        qubits_[qubit_id] = Qubit{};
        while (!qubits_.empty() && !qubits_.back().allocated)
            qubits_.pop_back();
    }

    void lock_hardware_id(size_t) override {}
    void unlock_hardware_id(size_t) override {}

    size_t get_num_qubits() const override { return qubits_.size(); }

    int measure(size_t qubit_id) override
    {
        Qubit &q = qubits_.at(qubit_id);
        if (q.x_basis)
            q.value = rng_.random_bit() ? 1 : 0;
        q.x_basis = false;
        return q.value;
    }

    double expectation_z(size_t qubit_id) override
    {
        const Qubit &q = qubits_.at(qubit_id);
        return q.x_basis ? 0.0 : (q.value ? -1.0 : 1.0);
    }

    void apply_h(size_t qubit_id) override
    {
        Qubit &q = qubits_.at(qubit_id);
        q.x_basis = !q.x_basis;
    }
    void apply_x(size_t qubit_id) override { qubits_.at(qubit_id).value ^= 1; }

    void apply_rz(size_t, double) override
    {
        throw std::logic_error("StubBackend: apply_rz 未建模（仅覆盖 QDS/BB84/货币用集）");
    }
    void apply_cnot(size_t, size_t) override
    {
        throw std::logic_error("StubBackend: apply_cnot 未建模（不支持纠缠协议，如 E91/CHSH）");
    }
    void apply_toffoli(size_t, size_t, size_t) override
    {
        throw std::logic_error("StubBackend: apply_toffoli 未建模");
    }
};

static void test_hash_family()
{
    using qchain::crypto::sha3_256;
    using qchain::crypto::hmac_sha256;
    using qchain::crypto::hkdf_sha256;
    using qchain::crypto::sha256;

    CHECK(to_hex(sha256(Bytes{'a', 'b', 'c'})) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(sha3_256(Bytes{})) ==
          "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");

    Bytes key(20, 0x0b);
    Bytes data = {'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};
    CHECK(to_hex(hmac_sha256(key, data)) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    Bytes ikm = {1, 2, 3}, salt = {0xaa, 0xbb}, info = {'i', 'n', 'f', 'o'};
    CHECK(hkdf_sha256(ikm, salt, info, 32) == hkdf_sha256(ikm, salt, info, 32));
}

static void test_mlkem()
{
    auto [ek, dk] = qchain::pqc::MlKem768::keygen();
    CHECK(ek.size() == 1184 && dk.size() == 2400);
    auto [ct, ss1] = qchain::pqc::MlKem768::encaps(ek);
    CHECK(ct.size() == 1088 && ss1.size() == 32);
    CHECK(qchain::pqc::MlKem768::decaps(dk, ct) == ss1);
}

static void test_mlkem_kat()
{
    using qchain::from_hex;
    using qchain::to_hex;

    auto upper = [](std::string s) {
        for (auto &c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };

    Bytes coins = from_hex(qchain_kat::D());
    Bytes z = from_hex(qchain_kat::Z());
    coins.insert(coins.end(), z.begin(), z.end());
    auto [ek, dk] = qchain::pqc::MlKem768::keygen_derand(coins);
    CHECK(upper(to_hex(ek)) == qchain_kat::EK());
    CHECK(upper(to_hex(dk)) == qchain_kat::DK());

    auto [ct, ss] = qchain::pqc::MlKem768::encaps_derand(
        from_hex(qchain_kat::ENC_EK()), from_hex(qchain_kat::M()));
    CHECK(upper(to_hex(ct)) == qchain_kat::C());
    CHECK(upper(to_hex(ss)) == qchain_kat::K());

    Bytes ss2 = qchain::pqc::MlKem768::decaps(
        from_hex(qchain_kat::ENC_DK()), from_hex(qchain_kat::C()));
    CHECK(upper(to_hex(ss2)) == qchain_kat::K());
}

static void test_mldsa()
{
    auto [pk, sk] = qchain::pqc::MlDsa65::keygen();
    CHECK(pk.size() == 1952 && sk.size() == 4032);
    Bytes msg = {'q', 'c', 'h', 'a', 'i', 'n'};
    Bytes sig = qchain::pqc::MlDsa65::sign(sk, msg);
    CHECK(sig.size() == 3309);
    CHECK(qchain::pqc::MlDsa65::verify(pk, msg, sig));
    Bytes bad = msg;
    bad[0] ^= 1;
    CHECK(!qchain::pqc::MlDsa65::verify(pk, bad, sig));
}

static void test_mldsa_kat()
{
    using qchain::from_hex;
    using qchain::to_hex;
    auto upper = [](std::string s) {
        for (auto &c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };

    auto [pk, sk] = qchain::pqc::MlDsa65::keygen_derand(from_hex(qchain_kat::DSA_SEED()));
    CHECK(upper(to_hex(pk)) == qchain_kat::DSA_PK());
    CHECK(upper(to_hex(sk)) == qchain_kat::DSA_SK());
}

static void test_sig_factory()
{
    using qchain::pqc::SigScheme;
    for (auto s : {SigScheme::ML_DSA, SigScheme::UOV, SigScheme::LAMPORT_OTS})
    {
        auto scheme = qchain::pqc::make_sig_scheme(s);
        auto [sk, pk] = scheme->keygen();
        Bytes msg = {'f', 'a', 'c', 't', 'o', 'r', 'y'};
        Bytes sig = scheme->sign(sk, msg);
        CHECK(scheme->verify(pk, msg, sig));
        Bytes bad = msg;
        bad[0] ^= 1;
        CHECK(!scheme->verify(pk, bad, sig));
    }
}

static void test_uov()
{
    auto [sk, pk] = qchain::pqc::UovSignature::keygen();
    Bytes msg = {'u', 'o', 'v', '-', 'm', 'q'};
    Bytes sig = qchain::pqc::UovSignature::sign(sk, msg);
    CHECK(sig.size() == qchain::pqc::UovParams::N);
    CHECK(qchain::pqc::UovSignature::verify(pk, msg, sig));
    Bytes bad = msg;
    bad[0] ^= 1;
    CHECK(!qchain::pqc::UovSignature::verify(pk, bad, sig));
}

static void test_lwe_kem()
{
    qchain::pqc::LweKem kem;
    auto [sk, pk] = kem.keygen();
    auto [ct, ss1] = kem.encaps(pk);
    CHECK(kem.decaps(sk, ct) == ss1);
}

static void test_csprng()
{
    qchain::Csprng a(Bytes{1, 2, 3, 4});
    qchain::Csprng b(Bytes{1, 2, 3, 4});
    qchain::Csprng c(Bytes{5, 6, 7, 8});
    CHECK(a.random_bytes(64) == b.random_bytes(64));
    CHECK(a.random_bytes(64) != c.random_bytes(64));
}

static void test_unicode_hash()
{
    using qhal::transliterate;
    using qhal::unicode_hash256;
    CHECK(transliterate("我") == "wo");
    CHECK(transliterate("我爱你") == "woaini");
    CHECK(unicode_hash256("我") == unicode_hash256("wo"));
    CHECK(unicode_hash256("你好") == unicode_hash256("nihao"));
    CHECK(transliterate("♥") == "heart");
}

static void test_causality_guard()
{
    qchain::causality::CausalityGuard guard;
    CHECK(guard.verify());
    for (int i = 0; i < 100; ++i)
        guard.step();
    CHECK(guard.verify_energy());
    CHECK(guard.verify_kaluza_klein());
    CHECK(guard.fingerprint() != qchain::causality::CausalityGuard().fingerprint());
}

static void test_spacetime_cipher()
{
    using qchain::spacetime_crypto::SpacetimeCipher;
    using qchain::spacetime_crypto::CipherMode;
    Bytes msg(1000);
    for (size_t i = 0; i < msg.size(); ++i)
        msg[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);

    SpacetimeCipher e1(12345, CipherMode::OFB), d1(12345, CipherMode::OFB);
    CHECK(d1.decrypt(e1.encrypt(msg)) == msg);
    SpacetimeCipher e2(12345, CipherMode::CTR), d2(12345, CipherMode::CTR);
    CHECK(d2.decrypt(e2.encrypt(msg)) == msg);
}

static void test_utxo_and_signature()
{
    using qchain::ledger::QChain;
    using qchain::ledger::Block;
    using qchain::ledger::Transaction;
    using qchain::ledger::TxInput;
    using qchain::ledger::TxOutput;
    using qchain::ledger::SignedTransaction;
    using qchain::pqc::LamportOts;

    LamportOts ots;
    auto [sk, pk] = ots.keygen();
    qchain::ledger::Address addr = qchain::crypto::derive_address(pk);
    qchain::ledger::Address addr2 = qchain::crypto::sha256(Bytes{'p', 'a', 'y'});
    auto verifier = [&](const SignedTransaction &st) { return st.verify(ots); };

    QChain chain;
    chain.init_genesis();

    SignedTransaction coinbase;
    coinbase.tx.outputs.push_back({addr, 100});
    coinbase.tx.timestamp = 1;
    const qchain::Hash256 coinbase_txid = coinbase.tx.txid();

    Block b1;
    b1.header.prev_hash = chain.tip().hash();
    b1.transactions.push_back(coinbase);
    b1.finalize();
    CHECK(chain.append(b1, verifier));
    CHECK(!chain.is_output_spent(coinbase_txid, 0));
    {
        auto utxos = chain.unspent_for(addr);
        CHECK(utxos.size() == 1 && utxos[0].second == 100);
    }

    Transaction tx;
    tx.inputs.push_back(TxInput{coinbase_txid, 0, {}});
    tx.outputs.push_back({addr2, 60});
    tx.outputs.push_back({addr, 40});
    tx.timestamp = 2;

    SignedTransaction st;
    st.tx = tx;
    st.public_key = pk;
    st.signature = ots.sign(sk, qchain::to_bytes(tx.txid()));
    CHECK(st.verify(ots));
    const qchain::Hash256 spend_txid = tx.txid();

    Block b2;
    b2.header.prev_hash = b1.hash();
    b2.transactions.push_back(st);
    b2.finalize();
    CHECK(chain.append(b2, verifier));
    CHECK(chain.is_output_spent(coinbase_txid, 0));
    {
        auto ua = chain.unspent_for(addr);
        auto ub = chain.unspent_for(addr2);
        CHECK(ua.size() == 1 && ua[0].second == 40);
        CHECK(ub.size() == 1 && ub[0].second == 60);
    }

    {
        Block b3;
        b3.header.prev_hash = b2.hash();
        SignedTransaction dup = st;
        dup.tx.timestamp = 3;
        dup.signature = ots.sign(sk, qchain::to_bytes(dup.tx.txid()));
        b3.transactions.push_back(dup);
        b3.finalize();
        CHECK(!chain.append(b3, verifier));
    }

    {
        Block bad;
        bad.header.prev_hash = b2.hash();
        Transaction fake;
        fake.inputs.push_back(TxInput{qchain::crypto::sha256(Bytes{'n', 'o', 'p', 'e'}), 0, {}});
        fake.outputs.push_back({addr, 1000});
        fake.timestamp = 3;
        SignedTransaction fst;
        fst.tx = fake;
        fst.public_key = pk;
        fst.signature = ots.sign(sk, qchain::to_bytes(fake.txid()));
        bad.transactions.push_back(fst);
        bad.finalize();
        CHECK(!chain.append(bad, verifier));
    }

    {
        Block bad;
        bad.header.prev_hash = b2.hash();
        Transaction val;
        val.inputs.push_back(TxInput{spend_txid, 1, {}});
        val.outputs.push_back({addr2, 100});
        val.timestamp = 4;
        SignedTransaction vst;
        vst.tx = val;
        vst.public_key = pk;
        vst.signature = ots.sign(sk, qchain::to_bytes(val.txid()));
        bad.transactions.push_back(vst);
        bad.finalize();
        CHECK(!chain.append(bad, verifier));
    }

    {
        Block bad;
        bad.header.prev_hash = b2.hash();
        Transaction t;
        t.inputs.push_back(TxInput{spend_txid, 1, {}});
        t.outputs.push_back({addr, 40});
        t.timestamp = 5;
        SignedTransaction sst;
        sst.tx = t;
        sst.public_key = pk;
        sst.signature = ots.sign(sk, qchain::to_bytes(t.txid()));
        sst.signature[0] ^= 0xFF;
        bad.transactions.push_back(sst);
        bad.finalize();
        CHECK(!chain.append(bad, verifier));
    }

    {
        LamportOts ots2;
        auto [sk2, pk2] = ots2.keygen();
        Block bad;
        bad.header.prev_hash = b2.hash();
        Transaction stolen;
        stolen.inputs.push_back(TxInput{spend_txid, 1, {}});
        stolen.outputs.push_back({addr2, 40});
        stolen.timestamp = 6;
        SignedTransaction sst;
        sst.tx = stolen;
        sst.public_key = pk2;
        sst.signature = ots2.sign(sk2, qchain::to_bytes(stolen.txid()));
        bad.transactions.push_back(sst);
        bad.finalize();
        CHECK(!chain.append(bad, [](const SignedTransaction &) { return true; }));
    }

    {
        Block bad;
        bad.header.prev_hash = b2.hash();
        Transaction first;
        first.inputs.push_back(TxInput{spend_txid, 1, {}});
        first.outputs.push_back({addr, 40});
        first.timestamp = 7;
        SignedTransaction fst;
        fst.tx = first;
        fst.public_key = pk;
        fst.signature = ots.sign(sk, qchain::to_bytes(first.txid()));
        SignedTransaction cb2;
        cb2.tx.outputs.push_back({addr, 1});
        cb2.tx.timestamp = 7;
        bad.transactions.push_back(fst);
        bad.transactions.push_back(cb2);
        bad.finalize();
        CHECK(!chain.append(bad, verifier));
    }

    {
        Block bad;
        bad.header.prev_hash = b2.hash();
        SignedTransaction cb1;
        cb1.tx.outputs.push_back({addr, 5});
        cb1.tx.timestamp = 8;
        SignedTransaction cb2;
        cb2.tx.outputs.push_back({addr, 5});
        cb2.tx.timestamp = 8;
        bad.transactions.push_back(cb1);
        bad.transactions.push_back(cb2);
        bad.finalize();
        CHECK(!chain.append(bad, verifier));
    }

    CHECK(chain.height() == 3);
    CHECK(chain.validate(verifier));
}

static void test_qds_subset_reveal()
{
    using qchain::qds::QuantumDigitalSignature;
    using qchain::qds::QuantumOneWayFunction;

    StubBackend backend;

    QuantumDigitalSignature::SigningKey kp;
    kp.secret = Bytes(32, 0x42);
    const size_t m = 64;
    kp.public_spec = QuantumOneWayFunction::evaluate(kp.secret, m);

    Bytes msg1 = {'q', 'c', 'h', 'a', 'i', 'n', '-', 'm', 's', 'g', '1'};
    Bytes msg2 = {'q', 'c', 'h', 'a', 'i', 'n', '-', 'm', 's', 'g', '2'};

    Bytes sig1 = QuantumDigitalSignature::sign(kp, msg1);
    std::vector<bool> mask1 = QuantumDigitalSignature::reveal_mask(msg1, m);
    std::vector<bool> mask2 = QuantumDigitalSignature::reveal_mask(msg2, m);
    size_t pop1 = 0, pop2 = 0;
    for (size_t i = 0; i < m; ++i)
    {
        if (mask1[i])
            ++pop1;
        if (mask2[i])
            ++pop2;
    }

    CHECK(sig1.size() == pop1);
    CHECK(sig1.size() > 0 && sig1.size() < m);

    CHECK(QuantumDigitalSignature::verify(&backend, kp.public_spec, msg1, sig1));

    CHECK(!QuantumDigitalSignature::verify(&backend, kp.public_spec, msg2, sig1));

    {
        Bytes trunc = sig1;
        trunc.pop_back();
        CHECK(!QuantumDigitalSignature::verify(&backend, kp.public_spec, msg1, trunc));
    }

    {
        Bytes sig2 = QuantumDigitalSignature::sign(kp, msg2);
        CHECK(sig2.size() == pop2);
        Bytes forged;
        size_t i1 = 0;
        for (size_t i = 0; i < m; ++i)
        {
            if (!mask2[i])
                continue;
            forged.push_back(mask1[i] ? sig1[i1] : qchain::Byte(0));
            if (mask1[i])
                ++i1;
        }
        CHECK(forged.size() == sig2.size());
        CHECK(!QuantumDigitalSignature::verify(&backend, kp.public_spec, msg2, forged));
    }

    {
        StubBackend b2;
        b2.allocate_qubits(3);
        auto ids = QuantumDigitalSignature::prepare_public_state(&b2, kp.public_spec);
        CHECK(ids.size() == m);
        CHECK(ids.front() == 3 && ids.back() == 3 + m - 1);
        for (size_t id : ids)
            b2.release_qubit(id);
        CHECK(b2.get_num_qubits() == 3);
    }
}

static void test_bb84_real_ids()
{
    StubBackend backend;
    backend.allocate_qubits(4);

    auto res = qchain::qkd::BB84::run(&backend, 64);
    CHECK(res.sifted_bits > 0);
    CHECK(res.error_rate == 0.0);
    CHECK(res.shared_key.size() == 32);
    CHECK(backend.get_num_qubits() == 4);
}

static void test_quantum_money()
{
    using qchain::token::QuantumMoney;
    const size_t n = 16;

    {
        StubBackend backend;
        backend.allocate_qubits(2);
        const size_t start = backend.get_num_qubits();
        auto note = QuantumMoney::mint(&backend, n, start);
        CHECK(note.size() == n && note.start_id == 2);
        CHECK(QuantumMoney::verify(&backend, note));
        QuantumMoney::destroy(&backend, note);
        CHECK(backend.get_num_qubits() == 2);
    }

    {
        StubBackend backend;
        backend.allocate_qubits(2);
        auto note = QuantumMoney::mint(&backend, n, backend.get_num_qubits());
        auto forged = note;
        forged.value[0] = !forged.value[0];
        CHECK(!QuantumMoney::verify(&backend, forged));
        QuantumMoney::destroy(&backend, note);
        CHECK(backend.get_num_qubits() == 2);
    }
}

int main()
{
    test_hash_family();
    test_mlkem();
    test_mlkem_kat();
    test_mldsa();
    test_mldsa_kat();
    test_uov();
    test_sig_factory();
    test_lwe_kem();
    test_csprng();
    test_unicode_hash();
    test_causality_guard();
    test_spacetime_cipher();
    test_utxo_and_signature();
    test_qds_subset_reveal();
    test_bb84_real_ids();
    test_quantum_money();

    if (g_failures == 0)
    {
        std::cout << "ALL QCHAIN SELFTESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " QCHAIN TEST(S) FAILED\n";
    return 1;
}