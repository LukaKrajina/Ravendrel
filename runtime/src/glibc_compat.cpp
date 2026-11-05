// ─────────────────────────────────────────────────────────────────────────────
// Linux / glibc 旧发行版兼容垫片
//
// 背景：在新发行版上编译（例如 Ubuntu 26.04 / glibc 2.43），链接器会把这些符号
// 绑定到 glibc *新增* 的符号版本：
//
//   fmod / fmodf                          → @@GLIBC_2.38（C23 libm 变更）
//   __isoc23_strto*                       → @@GLIBC_2.38（C23 strtol 重定向）
//   acosf / sqrtf                         → @@GLIBC_2.43（libm 新符号版本）
//   arc4random / arc4random_buf           → @@GLIBC_2.36（新引入，无旧版本）
//
// 动态加载器会校验「所需符号版本必须存在」，于是产物在旧发行版上（Ubuntu 22.04
// glibc 2.35 / Debian 12 glibc 2.36 / RHEL 9 glibc 2.34）会直接以
//   version `GLIBC_2.38' not found
// 启动失败，甚至 `ldd` 都报错。
//
// 做法：在本 DSO 内就地把这些「新名字」定义掉——
//   * libm/libc 旧版本仍存在的（fmod/sqrtf/acosf/strto*）：内部转调 @GLIBC_2.2.5；
//   * glibc 2.36 才引入的 arc4random 家族：用 getrandom(2) / /dev/urandom 自行实现。
// 于是产物只依赖 GLIBC_2.2.5 / 2.34。函数标记 hidden，避免对外做符号插入。
//
// 注意：本文件刻意只做手工声明、不包含系统头文件——就是为了定义 libc 已声明的
// 那些符号。也**不能**用 __GLIBC__ 做条件（不包含头文件时它不会被定义）。
// ─────────────────────────────────────────────────────────────────────────────

#if defined(__linux__) && !defined(__ANDROID__)

#define RVDL_HIDDEN __attribute__((visibility("hidden")))

extern "C" {

// ── 1. glibc 新符号版本：内部转调 @GLIBC_2.2.5 旧实现 ───────────────────
double        rvdl_glibc_fmod_225(double, double);
float         rvdl_glibc_fmodf_225(float, float);
float         rvdl_glibc_acosf_225(float);
float         rvdl_glibc_sqrtf_225(float);
long          rvdl_glibc_strtol_225(const char*, char**, int);
long long     rvdl_glibc_strtoll_225(const char*, char**, int);
unsigned long rvdl_glibc_strtoul_225(const char*, char**, int);
unsigned long long rvdl_glibc_strtoull_225(const char*, char**, int);
double        rvdl_glibc_strtod_225(const char*, char**);
float         rvdl_glibc_strtof_225(const char*, char**);
long double   rvdl_glibc_strtold_225(const char*, char**);

__asm__(".symver rvdl_glibc_fmod_225,fmod@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_fmodf_225,fmodf@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_acosf_225,acosf@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_sqrtf_225,sqrtf@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtol_225,strtol@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtoll_225,strtoll@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtoul_225,strtoul@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtoull_225,strtoull@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtod_225,strtod@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtof_225,strtof@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_strtold_225,strtold@GLIBC_2.2.5");

RVDL_HIDDEN double fmod(double a, double b) noexcept { return rvdl_glibc_fmod_225(a, b); }
RVDL_HIDDEN float  fmodf(float a, float b) noexcept { return rvdl_glibc_fmodf_225(a, b); }
RVDL_HIDDEN float  acosf(float x) noexcept { return rvdl_glibc_acosf_225(x); }
RVDL_HIDDEN float  sqrtf(float x) noexcept { return rvdl_glibc_sqrtf_225(x); }

RVDL_HIDDEN long __isoc23_strtol(const char* s, char** e, int b) noexcept
{
    return rvdl_glibc_strtol_225(s, e, b);
}
RVDL_HIDDEN long long __isoc23_strtoll(const char* s, char** e, int b) noexcept
{
    return rvdl_glibc_strtoll_225(s, e, b);
}
RVDL_HIDDEN unsigned long __isoc23_strtoul(const char* s, char** e, int b) noexcept
{
    return rvdl_glibc_strtoul_225(s, e, b);
}
RVDL_HIDDEN unsigned long long __isoc23_strtoull(const char* s, char** e, int b) noexcept
{
    return rvdl_glibc_strtoull_225(s, e, b);
}
RVDL_HIDDEN double __isoc23_strtod(const char* s, char** e) noexcept
{
    return rvdl_glibc_strtod_225(s, e);
}
RVDL_HIDDEN float __isoc23_strtof(const char* s, char** e) noexcept
{
    return rvdl_glibc_strtof_225(s, e);
}
RVDL_HIDDEN long double __isoc23_strtold(const char* s, char** e) noexcept
{
    return rvdl_glibc_strtold_225(s, e);
}

// ── 2. arc4random 家族：glibc 2.36 才引入，用 getrandom(2)/urandom 自行实现 ──
// （QChain 等需要密码学强度随机，这里走内核 CSPRNG）
long rvdl_glibc_syscall(long, ...) noexcept;
int  rvdl_glibc_open(const char*, int, ...) noexcept;
long rvdl_glibc_read(int, void*, unsigned long) noexcept;
int  rvdl_glibc_close(int) noexcept;

__asm__(".symver rvdl_glibc_syscall,syscall@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_open,open@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_read,read@GLIBC_2.2.5");
__asm__(".symver rvdl_glibc_close,close@GLIBC_2.2.5");

namespace
{
    // 填充密码学随机字节：优先 getrandom(2)，退回 /dev/urandom，最后弱回退兜底
    void rvdl_fill_random(void* buf, unsigned long len) noexcept
    {
        auto* p = static_cast<char*>(buf);
        unsigned long got = 0;

#if defined(__x86_64__)
        constexpr long kSysGetRandom = 318; // x86-64 上的 SYS_getrandom
        while (got < len)
        {
            const long n = rvdl_glibc_syscall(kSysGetRandom, p + got, len - got, 0);
            if (n <= 0) break; // ENOSYS/EINTR 等 → 走 urandom
            got += static_cast<unsigned long>(n);
        }
#endif
        if (got < len)
        {
            const int fd = rvdl_glibc_open("/dev/urandom", 0 /*O_RDONLY*/);
            if (fd >= 0)
            {
                while (got < len)
                {
                    const long n = rvdl_glibc_read(fd, p + got, len - got);
                    if (n <= 0) break;
                    got += static_cast<unsigned long>(n);
                }
                rvdl_glibc_close(fd);
            }
        }
        if (got < len)
        {
            // 极端兜底（不应发生）：地址 + 线性同余，至少避免死循环
            unsigned long long x = reinterpret_cast<unsigned long long>(p) ^ (got * 2654435761ULL);
            for (; got < len; ++got)
            {
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
                p[got] = static_cast<char>(x >> 33);
            }
        }
    }
} // namespace

RVDL_HIDDEN unsigned int arc4random(void) noexcept
{
    unsigned int v = 0;
    rvdl_fill_random(&v, sizeof(v));
    return v;
}

RVDL_HIDDEN void arc4random_buf(void* buffer, unsigned long size) noexcept
{
    rvdl_fill_random(buffer, size);
}

RVDL_HIDDEN unsigned int arc4random_uniform(unsigned int upper_bound) noexcept
{
    if (upper_bound < 2) return 0;
    const unsigned int bound = static_cast<unsigned int>(-upper_bound) % upper_bound; // 2^32 % upper
    unsigned int r = 0;
    do { r = arc4random(); } while (r < bound);
    return r % upper_bound;
}

}

#endif