# qchain 量子区块链手册

> 本手册覆盖 Ravendrel 的量子加密与量子区块链栈 **qchain**：后量子密码（PQC）、量子密钥分发（QKD）、量子数字签名（QDS）、量子拜占庭共识（QDBA）、量子货币与可编程量子代币。
> 相关代码位于 `runtime/include/qchain/`，语言层内置函数语法见 [rvdl 语言手册](./rvdl-language-manual.md)。
> qchain 使开发者既可用 **C++**（header-only 库），也可用 **rvdl 语言**（内置函数），还能通过 **daemon C ABI**（`rvdl_runtime_qchain_*`）创造各种各样的量子币种与量子区块链服务。

---

## 目录

1. [总览](#1-总览)
2. [研究背景](#2-研究背景)
3. [分层架构](#3-分层架构)
4. [原语层 primitives](#4-原语层-primitives)
5. [账本层 ledger](#5-账本层-ledger)
6. [共识层 consensus](#6-共识层-consensus)
7. [货币层 token](#7-货币层-token)
8. [服务层 QChainService](#8-服务层-qchainservice)
9. [rvdl 语言层的量子区块链](#9-rvdl-语言层的量子区块链)
10. [daemon C ABI](#10-daemon-c-abi)
11. [完整示例](#11-完整示例)

---

## 1. 总览

```
┌────────────────────────────────────────────────────────────────┐
│  rvdl 语言层（qchain_wallet / qchain_mint / qchain_qkd / ...）    │
│  daemon C ABI（rvdl_runtime_qchain_*，socket 协议 CMD_QCHAIN）  │
├────────────────────────────────────────────────────────────────┤
│  services/   QChainService（服务编排门面）                       │
│  ledger/     交易 · 区块 · 链（UTXO + 哈希链 + 量子纠缠链接）      │
│  consensus/  量子 PoW/PoS + QDBA 最终性                         │
│  token/      量子货币（Wiesner）· 可编程量子代币（类 ERC-20）     │
├────────────────────────────────────────────────────────────────┤
│  primitives/ pqc(格KEM+哈希签名) · qkd · qds · hash · causality  │
├────────────────────────────────────────────────────────────────┤
│  qhal（QM 真实量子机 / QVM 本地模拟器，4 种物理模态）            │
└────────────────────────────────────────────────────────────────┘
```

### 量子后端模态（qhal::HardwareModality）

qchain 的所有量子协议（QKD / QDS / QDBA / 量子货币）都运行在 `qhal::IQuantumBackend` 上，
后端有 **4 种物理模态**（通过 daemon 环境变量 `RVDL_BACKEND` 选择）：

| 模态 | 物理载体 | `RVDL_BACKEND` | 范式 |
| --- | --- | --- | --- |
| Superconducting | 超导微波电路（transmon） | `qvm`（默认）或 `qm` | 离散量子比特 |
| TrappedIon | 离子阱（QCCD） | `qm:ion` | 离散量子比特 |
| NeutralAtom | 中性原子（里德堡阻塞） | `qm:atom` | 离散量子比特 |
| **Photonic** | 光子芯片（硅 PIC，连续变量） | `qm:photon` | **连续变量高斯态** |

**光子后端（`qhal/PhotonicBackend.hpp`）** 是创新的第四范式：信息编码在光场正交分量
(x̂,p̂) 上，态为高斯态/完整梳态（GKP 编码），门为高斯门，并支持：

- **完整梳态（高斯包络叠加）**：GKP 逻辑态是 √π 晶格上的无限高斯峰叠加，逻辑门退化为
  峰索引的仿射/相位变换（位移 / Walsh-Hadamard / 求和门），使 GHZ 纠缠保真度达
  `Pz=Px=1`、`CHSH=2√2`（Bell 违背）。
- **连续变量高斯门**：`displace` / `squeeze` / `phase_shift` / `beam_splitter`。
- **高斯测量**：`homodyne`（正交分量）/ `photon_count`（PNR）。
- **光子噪声模型**：传输损耗 / 探测器效率 / 暗计数 / 有限压缩度。
- **纠缠验证**：`bell_verify()` 返回 Z/X 基一致率 + 纠缠见证 W（>0 ⟺ CHSH 违背）。

---

## 2. 研究背景

qchain 对齐量子区块链的三条研究主线：

| 主线 | 思想 | 代表方案 |
| --- | --- | --- |
| **后量子区块链** | 用抗量子密码替换易受 Shor 攻击的 RSA/ECDSA | NIST FIPS 203 ML-KEM（Kyber）、FIPS 204 ML-DSA（Dilithium）、FIPS 205 SLH-DSA（SPHINCS+）、FIPS 206 FN-DSA（Falcon） |
| **量子原生区块链** | 以纠缠 / QKD / 量子不可克隆为安全地基 | BB84/E91/诱骗态 QKD、Gottesman–Chuang QDS（arXiv:quant-ph/0105032）、GHZ 态 QDBA、Wiesner 量子货币 |
| **抗超时空与抗破解** | 用物理不变量守卫因果律，用语义/时空加密对抗破解 | 快子场因果哨兵（能量守恒 + KK 对称）、UnicodeHash256 语义哈希、SpacetimeCipher 时空加密 |

关键参考文献：

- *Quantum Blockchain Survey: Foundations, Trends, and Gaps*（arXiv:2507.13720）
- *QuantumShield-BC* / *PQ-PoETChain*（Sci. Rep. 2025）
- NIST 后量子密码标准化（FIPS 203/204/205/206）
- *Quantum Digital Signatures*（Gottesman & Chuang，arXiv:quant-ph/0105032）
- *Quantum detectable Byzantine agreement*（GHZ 态 DBA）

---

## 3. 分层架构

```
runtime/include/qchain/
├── common.hpp                    公共类型（Bytes / Hash256 / Rng / 哈希器）
├── qchain.hpp                    统一入口
├── primitives/
│   ├── hash.hpp                  SHA-256 + Merkle 树 + 地址派生
│   ├── pqc.hpp                   后量子密码（格 KEM + 哈希签名）
│   ├── qkd.hpp                   量子密钥分发（BB84 / E91 / 诱骗态）
│   ├── qds.hpp                   量子数字签名（Gottesman–Chuang）
│   ├── causality.hpp             快子场因果哨兵（防超时空）
│   └── spacetime_cipher.hpp      时空加密（OFB/CTR）
├── ledger/
│   ├── transaction.hpp           UTXO 交易 + 后量子签名
│   ├── block.hpp                 区块（哈希链 + 量子纠缠链接 + 因果链接）
│   └── chain.hpp                 链（追加 / 校验 / 分叉解决）
├── consensus/
│   ├── qba.hpp                   GHZ 态量子拜占庭一致
│   └── consensus.hpp             量子 PoW / PoS / QDBA 引擎
├── token/
│   └── quantum_coin.hpp          量子货币 + 可编程量子代币
├── services/
│   └── qchain_service.hpp        QChainService 服务编排门面
└── bridge/
    └── qchain_bridge.hpp         面向 rvdl 语言 / C 的 ABI 门面
```

统一入口：`#include "qchain/qchain.hpp"`。

---

## 4. 原语层 primitives

### 4.1 哈希（hash.hpp）

完整 SHA-256 + 默克尔树 + 量子安全地址派生：

```cpp
using qchain::crypto::sha256;
using qchain::crypto::MerkleTree;

qchain::Hash256 h = sha256(qchain::Bytes{'a','b','c'}); // 标准 SHA-256
qchain::Hash256 addr = qchain::crypto::derive_address(public_key); // 后量子地址
```

### 4.2 后量子密码（pqc.hpp / ml_kem.hpp / ml_dsa.hpp / sha3.hpp）

qchain 的密码原语层包含**完整 NIST 标准后量子密码**（真实移植，非骨架）：

| 组件 | 文件 | 说明 |
| --- | --- | --- |
| `MlKem768` | `ml_kem.hpp` | **完整 ML-KEM-768**（FIPS 203 / Kyber）：Montgomery NTT + 中心二项采样（CBD）+ FO 隐式拒绝，IND-CCA2 安全（移植自 pq-crystals/kyber，Public Domain / CC0） |
| `MlDsa65` | `ml_dsa.hpp` | **完整 ML-DSA-65**（FIPS 204 / Dilithium）：拒绝采样 + hint 分解 + `poly_challenge`，EUF-CMA 安全（移植自 pq-crystals/dilithium，Public Domain / CC0） |
| `sha3_256` / `sha3_512` / `shake128` / `shake256` | `sha3.hpp` | Keccak-f[1600] 海绵（FIPS 202），SHA3/SHAKE XOF（参考公有领域 tiny_sha3） |
| `IKem` | `pqc.hpp` | 密钥封装机制抽象（对齐 ML-KEM/Kyber） |
| `ISignatureScheme` | `pqc.hpp` | 数字签名抽象（对齐 ML-DSA/SLH-DSA/FN-DSA） |
| `LweKem` | `pqc.hpp` | Regev LWE 格 KEM（mini-Kyber 骨架） |
| `LamportOts` | `pqc.hpp` | Lamport 一次性签名（无条件安全） |
| `MerkleSignatureScheme` | `pqc.hpp` | XMSS 风格有状态多次签名 |

```cpp
// 完整 ML-KEM-768 密钥封装（确定性版本 keygen_derand / encaps_derand 可复现/KAT）
auto [pk, sk] = pqc::MlKem768::keygen();
auto [ct, ss1] = pqc::MlKem768::encaps(pk);
qchain::Bytes ss2 = pqc::MlKem768::decaps(sk, ct);      // ss1 == ss2

// 完整 ML-DSA-65 签名（sign 为确定性签名）
auto [dpk, dsk] = pqc::MlDsa65::keygen();
qchain::Bytes sig = pqc::MlDsa65::sign(dsk, msg);
bool ok = pqc::MlDsa65::verify(dpk, msg, sig);           // true

// 格 KEM 骨架（mini-Kyber）
pqc::LweKem kem(42);
auto [sk2, pk2] = kem.keygen();
auto [ct2, ssa] = kem.encaps(pk2);
qchain::Bytes ssb = kem.decaps(sk2, ct2);               // ssa == ssb
```

### 4.3 量子密钥分发（qkd.hpp）

所有协议在 `qhal::IQuantumBackend` 上实现（QVM 仿真或 QM 真实后端）：

```cpp
auto res = qchain::qkd::BB84::run(backend, 64);      // 制备-测量型
auto res = qchain::qkd::E91::run(backend, 32);        // 纠缠型
double S = qchain::qkd::E91::chsh_estimate(backend, 300); // CHSH 违背检测
auto res = qchain::qkd::DecoyBB84::run(backend, 64);  // 诱骗态（抗 PNS 攻击）
```

### 4.4 量子数字签名（qds.hpp）

Gottesman–Chuang 风格的量子单向函数签名：

```cpp
auto kp = qchain::qds::QuantumDigitalSignature::keygen(128);
qchain::Bytes sig = qchain::qds::QuantumDigitalSignature::sign(kp, message);
// 子集揭示：签名只包含 S(message) 上的比特（长度 ≈ m/2），不含完整公钥信息；
// 验证只需要量子公钥的制备描述（不含经典秘密），每次验证消耗一份公钥副本。
bool ok = qchain::qds::QuantumDigitalSignature::verify(backend, kp.public_spec, message, sig);
```

### 4.5 因果哨兵（causality.hpp，防超时空）

把 spacetime 的快子场（TachyonField）作为链上的「因果哨兵」：超时空篡改会破坏
快子场能量守恒（Noether 定理）或额外维 KK 质量谱对称，从而被拦截。

```cpp
qchain::causality::CausalityGuard guard;
guard.fingerprint();          // 因果指纹（写入区块头 causal_root）
guard.step();                 // 出块后推进快子场（因果链与区块链同频演化）
bool ok = guard.verify();     // 能量守恒 + KK 对称双重校验
```

### 4.6 时空加密（spacetime_cipher.hpp，抗破解）

快子场混沌演化生成 keystream，叠加 SHA-256 链式反馈（OFB / CTR 模式），并与后量子
格 KEM 组合成混合加密：

```cpp
using qchain::spacetime_crypto::SpacetimeCipher;
using qchain::spacetime_crypto::CipherMode;

SpacetimeCipher enc(seed, CipherMode::OFB);
qchain::Bytes ct = enc.encrypt(plaintext);   // 流加密
qchain::Bytes pt = enc.decrypt(ct);          // pt == plaintext
```

### 4.7 语义哈希（utils/UnicodeHash256.hpp，抗破解）

`qhal::unicode_hash256`：汉字/符号先音译为英文（基于 Unihan 全量拼音表，约 4.4 万条），
再 SHA-256，使「我」与「wo」、「你好」与「nihao」哈希一致：

```cpp
qhal::unicode_hash256("我") == qhal::unicode_hash256("wo");     // true
qhal::transliterate("我爱你") == "woaini";                       // true
```

---

## 5. 账本层 ledger

- **交易**：`TxInput` / `TxOutput` / `Transaction` / `SignedTransaction`（后量子签名）。
- **区块**：`BlockHeader`（版本、前哈希、默克尔根、时间戳、nonce、`qkd_entropy` 量子熵、`entanglement_root` 量子纠缠链接指纹）。
- **链**：`QChain`（创世块、追加校验、全链校验、分叉解决）。

```cpp
qchain::ledger::QChain chain;
chain.init_genesis();
// ... 构造区块（prev_hash + merkle_root 自洽）...
chain.append(block);
bool valid = chain.validate();
```

---

## 6. 共识层 consensus

| 组件 | 说明 |
| --- | --- |
| `QuantumByzantineAgreement` | GHZ 态可检测拜占庭一致（Z 基一致 / X 基奇偶校验） |
| `ConsensusEngine::quantum_pow` | 量子工作量证明（QKD 熵注入哈希挑战） |
| `ConsensusEngine::quantum_pos_select` | 量子权益证明（无偏抽签） |
| `ConsensusEngine::qdba_finalize` | GHZ 态确定性最终确认 |

```cpp
auto res = consensus::QuantumByzantineAgreement::run(backend, 3, {}, 20);
bool agreed = res.agreement; // 无拜占庭时 true
```

---

## 7. 货币层 token

| 组件 | 说明 |
| --- | --- |
| `QuantumMoney` | Wiesner 不可克隆量子钞票（序列号 + 随机基/值量子态） |
| `QuantumToken` | 可编程量子代币标准（类 ERC-20：铸币/转账/查余额） |

```cpp
auto note = token::QuantumMoney::mint(backend, 8, start_id); // 8 量子比特钞票
bool ok = token::QuantumMoney::verify(backend, note);         // 破坏性测量验证
```

---

## 8. 服务层 QChainService

`QChainService` 是顶层门面，两种后端接入模式：

```cpp
// 1. 独立创建（C++ 开发者，拥有后端生命周期）
qchain::QChainService svc(qchain::QChainService::Config{});

// 2. 复用外部后端（rvdl 桥接用 runtime 的 global_qm）
qchain::QChainService svc(global_qm, qchain::QChainService::Config{});

auto &alice = svc.create_wallet();      // 后量子钱包
svc.mint(alice, 1000000);               // 铸币（签名交易）
svc.transfer(alice, bob_addr, 500);     // 转账
svc.mine_block();                       // 出块
svc.verify_chain();                     // 验链
svc.establish_qkd(64);                  // BB84 QKD
```

按地址操作（rvdl 桥接用）：`mint_to_address` / `transfer_addresses` / `balance_of_address`。

---

## 9. rvdl 语言层的量子区块链

qchain 以 11 个内置函数暴露给 rvdl 语言，让开发者用几行 rvdl 编写量子币种与量子区块链服务：

| 内置函数 | 签名 | 返回 | 说明 |
| --- | --- | --- | --- |
| `qchain_wallet` | `qchain_wallet()` | `string` | 创建后量子钱包，返回量子安全地址 |
| `qchain_mint` | `qchain_mint(string, uint64)` | `void` | 铸币到指定地址 |
| `qchain_transfer` | `qchain_transfer(string, string, uint64)` | `int32` | 地址间转账 |
| `qchain_balance` | `qchain_balance(string)` | `uint64` | 查询余额 |
| `qchain_mine` | `qchain_mine()` | `int32` | 量子 PoW 出块；成功返回链高度，失败返回 `-1`（内存池保留，交易不丢失） |
| `qchain_height` | `qchain_height()` | `int32` | 链高度 |
| `qchain_verify` | `qchain_verify()` | `int32` | 验链 |
| `qchain_qkd` | `qchain_qkd(int32)` | `string` | BB84 QKD，返回共享密钥 |
| `qchain_qdba` | `qchain_qdba(int32)` | `int32` | 量子拜占庭共识 |
| `qchain_coin_mint` | `qchain_coin_mint(int32)` | `QObject` | 铸不可克隆量子钞票 |
| `qchain_coin_verify` | `qchain_coin_verify(QObject)` | `int32` | 验证量子钞票 |

完整示例见 [`examples/qchain_coin.rvdl`](../examples/qchain_coin.rvdl)：

```rvdl
@layer(time=0, thread=0, coord=(0))
int32 rvdl_main() {
    string alice = qchain_wallet();
    string bob   = qchain_wallet();
    qchain_mint(alice, 1000000);
    int32 ok = qchain_transfer(alice, bob, 500);
    uint64 bal = qchain_balance(alice);
    int32 height = qchain_mine();
    int32 valid = qchain_verify();
    string key = qchain_qkd(64);
    QObject coin = qchain_coin_mint(8);
    int32 coin_ok = qchain_coin_verify(coin);
    int32 agreed = qchain_qdba(5);
    return ok;
}
```

---

## 10. daemon C ABI

除 rvdl 语言外，外部进程可通过 daemon 的 C ABI 直接调用量子区块链服务：

```c
// RuntimeApi.h 导出
rvdl_runtime_qchain_wallet(rt);
rvdl_runtime_qchain_mint(rt, addr, amount);
rvdl_runtime_qchain_transfer(rt, from, to, amount);
rvdl_runtime_qchain_balance(rt, addr);
rvdl_runtime_qchain_mine(rt);
rvdl_runtime_qchain_height(rt);
rvdl_runtime_qchain_verify(rt);
rvdl_runtime_qchain_qkd(rt, rounds);
rvdl_runtime_qchain_qdba(rt, parties);
```

daemon socket 协议新增 `CMD_QCHAIN = 0x10`，payload 为 `"op args..."` 文本；前台 stdin 协议新增 `QCHAIN <op> <args>` 文本命令。可用操作：`wallet` / `mint` / `transfer` / `balance` / `mine` / `height` / `verify` / `qkd` / `qdba`。

---

## 11. 完整示例

```bash
# 前台模式（stdin/stdout）：先启动 runtime，逐行输入命令
./runtime
# QCHAIN wallet
# QCHAIN mint <addr> 1000000
# QCHAIN mine
# QCHAIN verify

# 运行 rvdl 量子币示例（需先 ./runtime --daemon）
rvdl run examples/qchain_coin.rvdl
```

> 注意：构建需按 [README「构建前必读」](../README.md#-构建前必读) 配置本机 LLVM / Kokkos / CUDA 路径；量子货币与 QDBA 的量子态操作依赖 `global_qm` 后端（默认 QVM 仿真）。
