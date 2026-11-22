# qchain 量子区块链手册

qchain 是 Ravendrel 的量子加密与区块链栈：后量子密码（ML-KEM / ML-DSA）+ QKD + 量子数字签名 + GHZ 拜占庭共识 + 不可克隆量子货币。三种用法：**C++ 库**、**rvdl 内置函数**、**daemon / stdin 协议**。

> 量子货币和 QDBA 依赖 `global_qm` 后端（默认 QVM 仿真）；真实量子机通过 `RVDL_BACKEND` 配置。

## 目录

- [rvdl 内置函数（最常用）](#rvdl-内置函数最常用)
- [C++ 库速查](#c-库速查)
- [daemon / stdin 协议](#daemon--stdin-协议)
- [量子后端模态](#量子后端模态)

---

## rvdl 内置函数（最常用）

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    string alice = qchain_wallet();
    string bob   = qchain_wallet();
    qchain_mint(alice, 1000000);
    int32 ok     = qchain_transfer(alice, bob, 500);
    uint64 bal   = qchain_balance(alice);
    int32 height = qchain_mine();
    int32 valid  = qchain_verify();
    string key   = qchain_qkd(64);
    QObject coin = qchain_coin_mint(8);
    int32 coin_ok = qchain_coin_verify(coin);
    int32 agreed = qchain_qdba(5);
    return ok;
}
```

| 函数 | 返回 | 含义 |
| --- | --- | --- |
| `qchain_wallet()` | `string` | 创建后量子钱包，返回量子安全地址 |
| `qchain_mint(addr, amount)` | `void` | 铸币到地址 |
| `qchain_transfer(from, to, amount)` | `int32` | 地址间转账 |
| `qchain_balance(addr)` | `uint64` | 查询余额 |
| `qchain_mine()` | `int32` | 量子 PoW 出块，成功返回链高度，失败 −1（内存池保留） |
| `qchain_height()` / `qchain_verify()` | `int32` | 链高度 / 验链 |
| `qchain_qkd(bits)` | `string` | BB84 QKD，返回共享密钥 |
| `qchain_qdba(n)` | `int32` | GHZ 态量子拜占庭共识 |
| `qchain_coin_mint(n)` / `qchain_coin_verify(coin)` | `QObject` / `int32` | Wiesner 不可克隆钞票的铸造 / 验证 |

哈希与密码：`qchain_sha3(string)`、`qchain_hmac(string,string)`、`qchain_hash_unicode(string)`（汉字 → 拼音 → SHA-256，使「我」与「wo」哈希一致）、`qchain_sign(string)` / `qchain_sign_verify(msg,sig)` / `qchain_sign_pubkey()`（ML-DSA-65）、`qchain_mlkem_encaps(pubkey)` / `qchain_mlkem_decaps(privkey,ct)`（ML-KEM-768）、`qchain_causal_verify()`（因果哨兵）、`qchain_cipher_encrypt(seed,text)` / `qchain_cipher_decrypt(seed,ct)`（时空加密）。

---

## C++ 库速查

统一入口 `#include "qchain/qchain.hpp"`，实现位于 `runtime/include/qchain/`（header-only）。

### 原语层

```cpp
// 哈希与地址
qchain::Hash256 h = qchain::crypto::sha256(qchain::Bytes{'a','b','c'});
qchain::Hash256 addr = qchain::crypto::derive_address(public_key);

// 后量子密码（完整 NIST FIPS 203/204 移植）
auto [pk, sk]  = qchain::pqc::MlKem768::keygen();
auto [ct, ss1] = qchain::pqc::MlKem768::encaps(pk);
auto ss2 = qchain::pqc::MlKem768::decaps(sk, ct);      // ss1 == ss2
auto [dpk, dsk] = qchain::pqc::MlDsa65::keygen();
auto sig = qchain::pqc::MlDsa65::sign(dsk, msg);
bool ok = qchain::pqc::MlDsa65::verify(dpk, msg, sig);

// QKD 三协议 + CHSH
auto r1 = qchain::qkd::BB84::run(backend, 64);
auto r2 = qchain::qkd::E91::run(backend, 32);
double S = qchain::qkd::E91::chsh_estimate(backend, 300);
auto r3 = qchain::qkd::DecoyBB84::run(backend, 64);
```

| 组件 | 头文件 | 用途 |
| --- | --- | --- |
| `sha256` / `MerkleTree` | `primitives/hash.hpp` | SHA-256 + 默克尔树 + 地址派生 |
| `MlKem768` / `MlDsa65` | `primitives/ml_kem.hpp` / `ml_dsa.hpp` | 完整 ML-KEM-768 / ML-DSA-65 |
| `sha3_256` / `shake128` 等 | `primitives/sha3.hpp` | Keccak-f[1600] |
| `BB84` / `E91` / `DecoyBB84` | `primitives/qkd.hpp` | QKD 三协议 |
| `QuantumDigitalSignature` | `primitives/qds.hpp` | Gottesman–Chuang QDS（子集揭示，每次验证消耗一份量子公钥副本） |
| `CausalityGuard` | `primitives/causality.hpp` | 快子场因果哨兵：防超时空篡改 |
| `SpacetimeCipher` | `primitives/spacetime_cipher.hpp` | 时空加密 OFB / CTR |
| `LweKem` | `primitives/pqc.hpp` | Regev LWE 格 KEM（mini-Kyber 骨架） |

### 账本 / 共识 / 货币 / 服务

```cpp
qchain::ledger::QChain chain;
chain.init_genesis();
chain.append(block);
bool valid = chain.validate();

auto res = qchain::consensus::QuantumByzantineAgreement::run(backend, 3, {}, 20);
bool agreed = res.agreement;

auto note = qchain::token::QuantumMoney::mint(backend, 8, start_id);
bool ok = qchain::token::QuantumMoney::verify(backend, note);  // 破坏性测量

qchain::QChainService svc(qchain::QChainService::Config{});
auto& alice = svc.create_wallet();
svc.mint(alice, 1000000);
svc.transfer(alice, bob_addr, 500);
svc.mine_block();
svc.verify_chain();
svc.establish_qkd(64);
```

- **账本**（`ledger/`）：`Transaction`（后量子签名）、`BlockHeader`（含 `qkd_entropy` / `entanglement_root` / 因果链接）、`QChain`（创世、追加、验证、分叉解决）。
- **共识**（`consensus/`）：`QuantumByzantineAgreement`（GHZ 态 Z 基一致 + X 基奇偶）、`ConsensusEngine`（`quantum_pow` / `quantum_pos_select` / `qdba_finalize`）。
- **货币**（`token/quantum_coin.hpp`）：`QuantumMoney`（Wiesner 不可克隆钞票）、`QuantumToken`（可编程代币）。
- **服务**（`services/qchain_service.hpp`）：`create_wallet` / `mint` / `transfer` / `mine_block` / `verify_chain` / `establish_qkd`。

---

## daemon / stdin 协议

C ABI（`RuntimeApi.h`）：`rvdl_runtime_qchain_wallet/mint/transfer/balance/mine/height/verify/qkd/qdba`。

socket：`CMD_QCHAIN=0x10`，payload `"op args..."`。

stdin（先启动 `./runtime`）：

```
QCHAIN wallet
QCHAIN mint <addr> 1000000
QCHAIN mine
QCHAIN verify
```

运行示例脚本（需先 `./runtime --daemon`）：

```bash
rvdl run examples/qchain_coin.rvdl
```

---

## 量子后端模态

通过 `RVDL_BACKEND` 选择：

| 值 | 后端 |
| --- | --- |
| `qvm`（默认）/ `qm` | 超导 |
| `qm:ion` | 离子阱 |
| `qm:atom` | 中性原子 |
| `qm:photon` | 光子（连续变量高斯态、完整 GKP 梳态、`bell_verify()` 纠缠验证） |

语言函数全表见 [语言参考手册](rvdl-language-manual.md#内置函数速查)。