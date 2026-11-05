#pragma once
//
// qchain :: 公共基础类型与工具
//
// 量子加密 + 量子区块链底层（qchain）的共享基础：
//   • 字节/哈希类型定义（Bytes / Hash256）
//   • 可注入种子的确定性 PRNG（Rng），供 KEM / 签名 / QKD / 共识等需要随机性的环节复用
//   • 十六进制编解码
//
// 设计目标：让其他开发者基于 qchain 创造各种量子币种与量子区块链服务时，
// 只需依赖这一份最小公共头，即可复用统一的类型与随机源约定。
//
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdexcept>

// Windows SDK 的 rpcndr.h 将 small 定义为空宏（IDL 属性），
// winsock2.h/windows.h 间接引入后污染本头的 small() 方法名。
#ifdef small
#undef small
#endif

namespace qchain
{

    using Byte = uint8_t;
    using Bytes = std::vector<Byte>;
    using Hash256 = std::array<Byte, 32>;

    // Hash256 的 FNV-1a 哈希器。std::hash<std::array<T,N>> 并非标准库强制提供
    // （MSVC 不提供），故需自定义以在 unordered_map / unordered_set 中作为键使用。
    struct Hash256Hasher
    {
        size_t operator()(const Hash256 &h) const noexcept
        {
            size_t hash = 14695981039346656037ULL; // FNV offset basis
            for (Byte b : h)
            {
                hash ^= b;
                hash *= 1099511628211ULL; // FNV prime
            }
            return hash;
        }
    };

    // ─── 可注入种子的确定性 PRNG ─────────────────────────────────────
    //
    // 底层使用 mt19937_64。默认以系统熵（random_device）播种；
    // 需要可复现的测试 / 审计 / 单元验证时可显式传入种子。
    class Rng
    {
    private:
        std::mt19937_64 gen;

    public:
        Rng() { std::random_device rd; gen.seed(rd()); }
        explicit Rng(uint64_t seed) { gen.seed(seed); }

        uint64_t next_u64() { return gen(); }

        // [0, bound) 均匀整数
        uint64_t uniform(uint64_t bound)
        {
            if (bound == 0)
                return 0;
            std::uniform_int_distribution<uint64_t> d(0, bound - 1);
            return d(gen);
        }

        double uniform_real() { return std::uniform_real_distribution<double>(0.0, 1.0)(gen); }

        // 中心小整数采样（{-1,0,1} 二项式近似），用于 LWE 秘密/误差向量。
        int small()
        {
            uint64_t u = uniform(100);
            if (u < 25)
                return -1;
            if (u < 75)
                return 0;
            return 1;
        }

        Bytes random_bytes(size_t n)
        {
            Bytes b(n);
            for (auto &x : b)
                x = Byte(uniform(256));
            return b;
        }

        bool random_bit() { return uniform(2) == 1; }
    };

    // ─── 常量时间比较（防时序侧信道）────────────────────────────────
    // 密码学比较必须常量时间，否则攻击者可通过计时推断密钥/哈希前缀。
    inline bool constant_time_equal(const void *a, const void *b, size_t n)
    {
        const Byte *pa = static_cast<const Byte *>(a);
        const Byte *pb = static_cast<const Byte *>(b);
        Byte diff = 0;
        for (size_t i = 0; i < n; ++i)
            diff = static_cast<Byte>(diff | (pa[i] ^ pb[i]));
        return diff == 0;
    }
    inline bool constant_time_equal(const Bytes &a, const Bytes &b)
    {
        if (a.size() != b.size())
            return false; // 长度非秘密，可提前返回
        return constant_time_equal(a.data(), b.data(), a.size());
    }
    inline bool constant_time_equal(const Hash256 &a, const Hash256 &b)
    {
        return constant_time_equal(a.data(), b.data(), a.size());
    }

    // ─── 安全清零（防密钥残留 + 防编译器优化消除）────────────────────
    inline void secure_wipe(void *ptr, size_t n)
    {
        volatile Byte *p = static_cast<volatile Byte *>(ptr);
        while (n--)
            *p++ = 0;
    }
    inline void secure_wipe(Bytes &b)
    {
        if (!b.empty())
            secure_wipe(b.data(), b.size());
    }

    // ─── 密码学安全伪随机数发生器（ChaCha20 CSPRNG）─────────────────
    //
    // 生产级密码学（密钥生成 / KEM / 签名）必须使用 CSPRNG，mt19937 可被预测。
    // 本实现为 ChaCha20 流密码内核 + 系统熵（std::random_device）播种，
    // 提供 256 位安全强度；可在需要可复现时用显式 seed 构造（仅测试/审计用）。
    class Csprng
    {
    private:
        uint32_t state_[16];
        uint32_t block_[16];
        size_t block_pos_ = 16;

        static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

        static void quarter_round(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d)
        {
            a += b; d ^= a; d = rotl32(d, 16);
            c += d; b ^= c; b = rotl32(b, 12);
            a += b; d ^= a; d = rotl32(d, 8);
            c += d; b ^= c; b = rotl32(b, 7);
        }

        void init(const uint32_t key[8], const uint32_t nonce[3])
        {
            static const uint32_t constants[4] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
            for (int i = 0; i < 4; ++i)
                state_[i] = constants[i];
            for (int i = 0; i < 8; ++i)
                state_[4 + i] = key[i];
            state_[12] = 0; // counter 低 32 位
            state_[13] = 0; // counter 高 32 位
            for (int i = 0; i < 3; ++i)
                state_[14 + i] = nonce[i];
            block_pos_ = 16; // 触发首块生成
        }

        void generate_block()
        {
            uint32_t x[16];
            for (int i = 0; i < 16; ++i)
                x[i] = state_[i];
            for (int r = 0; r < 10; ++r) // 10 double rounds = 20 rounds
            {
                quarter_round(x[0], x[4], x[8], x[12]);
                quarter_round(x[1], x[5], x[9], x[13]);
                quarter_round(x[2], x[6], x[10], x[14]);
                quarter_round(x[3], x[7], x[11], x[15]);
                quarter_round(x[0], x[5], x[10], x[15]);
                quarter_round(x[1], x[6], x[11], x[12]);
                quarter_round(x[2], x[7], x[8], x[13]);
                quarter_round(x[3], x[4], x[9], x[14]);
            }
            for (int i = 0; i < 16; ++i)
                block_[i] = x[i] + state_[i];
            if (++state_[12] == 0)
                ++state_[13];
            block_pos_ = 0;
        }

        uint32_t next_u32()
        {
            if (block_pos_ >= 16)
                generate_block();
            return block_[block_pos_++];
        }

    public:
        // 系统熵播种（生产用）。
        Csprng()
        {
            uint32_t key[8], nonce[3];
            std::random_device rd;
            for (auto &k : key)
                k = rd();
            for (auto &n : nonce)
                n = rd();
            init(key, nonce);
        }

        // 显式 seed（仅测试/审计用，可复现）。
        explicit Csprng(const Bytes &seed)
        {
            uint32_t key[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            uint32_t nonce[3] = {0, 0, 0};
            for (size_t i = 0; i < seed.size(); ++i)
            {
                size_t idx = i % 44; // 32 字节 key + 12 字节 nonce
                Byte b = seed[i];
                if (idx < 32)
                    key[idx / 4] |= uint32_t(b) << (8 * (idx % 4));
                else
                    nonce[(idx - 32) / 4] |= uint32_t(b) << (8 * ((idx - 32) % 4));
            }
            init(key, nonce);
        }

        Bytes random_bytes(size_t n)
        {
            Bytes b(n);
            for (size_t i = 0; i < n; ++i)
                b[i] = static_cast<Byte>(next_u32() & 0xFF);
            return b;
        }

        uint64_t next_u64()
        {
            uint64_t lo = next_u32(), hi = next_u32();
            return (hi << 32) | lo;
        }

        // [0, bound) 均匀整数（rejection sampling 避免模偏差）。
        uint64_t uniform(uint64_t bound)
        {
            if (bound == 0)
                return 0;
            uint64_t threshold = (-bound) % bound;
            for (;;)
            {
                uint64_t r = next_u64();
                if (r >= threshold)
                    return r % bound;
            }
        }

        bool random_bit() { return (next_u32() & 1) != 0; }

        // 中心二项式分布 CBD_η：sum_{i=1..η}(b_i - b'_i)，用于 LWE 秘密/误差采样。
        int cbd(int eta)
        {
            int x = 0;
            for (int i = 0; i < eta; ++i)
                x += (next_u32() & 1);
            for (int i = 0; i < eta; ++i)
                x -= (next_u32() & 1);
            return x;
        }
    };

    // ─── 十六进制编解码 ──────────────────────────────────────────────

    inline std::string to_hex(const Bytes &data)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (Byte b : data)
            oss << std::setw(2) << static_cast<int>(b);
        return oss.str();
    }

    inline std::string to_hex(const Hash256 &h)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (Byte b : h)
            oss << std::setw(2) << static_cast<int>(b);
        return oss.str();
    }

    // Hash256 → Bytes（用于把哈希作为签名消息传入 Bytes 接口）。
    inline Bytes to_bytes(const Hash256 &h) { return Bytes(h.begin(), h.end()); }

    inline Bytes from_hex(const std::string &s)
    {
        if (s.size() % 2 != 0)
            throw std::invalid_argument("qchain::from_hex: odd-length hex string");
        // DEBUG(临时): 打印非法输入全文，定位无效来源。
        for (char ch : s)
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            {
                std::cerr << "[DEBUG from_hex] input(len=" << s.size() << ") = \"" << s << "\"\n";
                break;
            }
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            throw std::invalid_argument("qchain::from_hex: invalid hex char");
        };
        Bytes out;
        out.reserve(s.size() / 2);
        for (size_t i = 0; i < s.size(); i += 2)
            out.push_back(Byte((nib(s[i]) << 4) | nib(s[i + 1])));
        return out;
    }

}