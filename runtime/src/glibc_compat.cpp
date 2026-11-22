#if defined(__linux__) && !defined(__ANDROID__)

#define RVDL_HIDDEN __attribute__((visibility("hidden")))

extern "C" {

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
    void rvdl_fill_random(void* buf, unsigned long len) noexcept
    {
        auto* p = static_cast<char*>(buf);
        unsigned long got = 0;

#if defined(__x86_64__)
        constexpr long kSysGetRandom = 318;
        while (got < len)
        {
            const long n = rvdl_glibc_syscall(kSysGetRandom, p + got, len - got, 0);
            if (n <= 0) break;
            got += static_cast<unsigned long>(n);
        }
#endif
        if (got < len)
        {
            const int fd = rvdl_glibc_open("/dev/urandom", 0 );
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
            unsigned long long x = reinterpret_cast<unsigned long long>(p) ^ (got * 2654435761ULL);
            for (; got < len; ++got)
            {
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
                p[got] = static_cast<char>(x >> 33);
            }
        }
    }
}

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
    const unsigned int bound = static_cast<unsigned int>(-upper_bound) % upper_bound;
    unsigned int r = 0;
    do { r = arc4random(); } while (r < bound);
    return r % upper_bound;
}

}

#endif