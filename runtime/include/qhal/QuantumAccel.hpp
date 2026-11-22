#pragma once
#include <Kokkos_Core.hpp>

#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#else
#include <dlfcn.h>
#endif

namespace rvdl
{
    namespace accel
    {
        enum class Vendor { Unknown, Nvidia, Amd, Intel, Cpu };
        enum class Backend { Cpu, Cuda, Hip, Sycl };

        inline const char *vendor_name(Vendor v)
        {
            switch (v)
            {
                case Vendor::Nvidia: return "NVIDIA";
                case Vendor::Amd: return "AMD";
                case Vendor::Intel: return "Intel";
                case Vendor::Cpu: return "CPU";
                default: return "Unknown";
            }
        }
        inline const char *backend_name(Backend b)
        {
            switch (b)
            {
                case Backend::Cuda: return "CUDA";
                case Backend::Hip: return "HIP";
                case Backend::Sycl: return "SYCL";
                default: return "OpenMP/Serial";
            }
        }

        inline Backend compiled_backend()
        {
#if defined(KOKKOS_ENABLE_CUDA)
            return Backend::Cuda;
#elif defined(KOKKOS_ENABLE_HIP)
            return Backend::Hip;
#elif defined(KOKKOS_ENABLE_SYCL)
            return Backend::Sycl;
#else
            return Backend::Cpu;
#endif
        }

        struct DeviceInfo
        {
            Vendor vendor = Vendor::Unknown;
            Backend backend = Backend::Cpu;
            std::string name;
            int count = 0;
            uint64_t memory_bytes = 0;
            bool usable = false;
            std::string reason;
        };

        namespace detail
        {
            inline bool exists(const char *p)
            {
                std::error_code ec;
                return std::filesystem::exists(std::filesystem::path(p), ec);
            }
            inline bool has_env(const char *k)
            {
                const char *v = std::getenv(k);
                return v && v[0];
            }

            inline bool probe_nvidia(DeviceInfo &out)
            {
#if defined(_WIN32)
                const bool ok = exists("C:/Windows/System32/nvcuda.dll") || has_env("CUDA_PATH");
                if (!ok) return false;
                out.name = "NVIDIA CUDA (nvcuda.dll / CUDA_PATH)";
#else
                const bool ok = exists("/dev/nvidiactl") || exists("/dev/nvidia0") || has_env("CUDA_PATH");
                if (!ok) return false;
                out.name = "NVIDIA CUDA device";
#endif
                out.vendor = Vendor::Nvidia;
                out.count = 1;
                return true;
            }

            inline bool probe_amd(DeviceInfo &out)
            {
#if defined(_WIN32)
                const bool ok = exists("C:/Windows/System32/amdhip64.dll") || has_env("HIP_PATH");
#else
                const bool ok = exists("/dev/kfd") || exists("/dev/dri") || has_env("ROCM_PATH");
#endif
                if (!ok) return false;
                out.vendor = Vendor::Amd;
                out.count = 1;
                out.name = "AMD ROCm / HIP device";
                return true;
            }

            inline bool probe_intel(DeviceInfo &out)
            {
#if defined(_WIN32)
                const bool ok = exists("C:/Windows/System32/ze_loader.dll") ||
                                exists("C:/Windows/System32/OpenCL.dll");
#else
                const bool ok = exists("/dev/dri") ||
                                exists("/usr/lib/x86_64-linux-gnu/libze_loader.so.1");
#endif
                if (!ok) return false;
                out.vendor = Vendor::Intel;
                out.count = 1;
                out.name = "Intel Level Zero GPU";
                return true;
            }
        }

        struct Detection
        {
            DeviceInfo primary;
            std::vector<DeviceInfo> others;
            bool nvidia = false, amd = false, intel = false;
        };

        inline Detection detect()
        {
            Detection d;
            std::vector<DeviceInfo> found;
            DeviceInfo n, a, i;
            d.nvidia = detail::probe_nvidia(n); if (d.nvidia) found.push_back(n);
            d.amd = detail::probe_amd(a);       if (d.amd) found.push_back(a);
            d.intel = detail::probe_intel(i);   if (d.intel) found.push_back(i);
            if (found.empty())
            {
                d.primary.vendor = Vendor::Cpu;
                d.primary.name = "host (no GPU runtime found)";
                return d;
            }
            d.primary = found.front();
            for (size_t k = 1; k < found.size(); ++k) d.others.push_back(found[k]);
            return d;
        }

        inline DeviceInfo select(bool force_gpu, bool allow_cpu_fallback = true)
        {
            Detection d = detect();
            std::vector<DeviceInfo> all;
            if (d.primary.vendor != Vendor::Cpu) all.push_back(d.primary);
            for (const auto &o : d.others) all.push_back(o);

            const Backend compiled = compiled_backend();
            auto backend_for = [](Vendor v) -> Backend
            {
                if (v == Vendor::Nvidia) return Backend::Cuda;
                if (v == Vendor::Amd) return Backend::Hip;
                if (v == Vendor::Intel) return Backend::Sycl;
                return Backend::Cpu;
            };

            for (const auto &x : all)
            {
                if (compiled != Backend::Cpu && backend_for(x.vendor) == compiled)
                {
                    DeviceInfo out = x;
                    out.backend = compiled;
                    out.usable = true;
                    out.reason = std::string("启用 ") + backend_name(compiled) + "：" + out.name;
                    return out;
                }
            }

            DeviceInfo out = d.primary;
            out.backend = compiled;
            out.usable = false;
            if (all.empty())
            {
                out.reason = "未发现 GPU 运行时，使用 CPU/OpenMP";
            }
            else if (compiled == Backend::Cpu)
            {
                out.reason = std::string("检测到 ") + vendor_name(out.vendor) +
                    " GPU，但本二进制只编了 CPU 后端；请用启用 " +
                    (out.vendor == Vendor::Nvidia ? "CUDA" : out.vendor == Vendor::Amd ? "HIP" : "SYCL") +
                    " 的 Kokkos 重新编译";
            }
            else
            {
                out.reason = std::string("检测到 ") + vendor_name(out.vendor) +
                    " 等 GPU，但没有与本二进制 " + backend_name(compiled) +
                    " 后端匹配的厂商（一个 Kokkos 二进制只带一个 GPU 后端），回退 CPU/OpenMP";
            }

            if (force_gpu && !out.usable && !allow_cpu_fallback)
                out.reason += "（--device gpu 要求强制 GPU）";
            return out;
        }

        inline bool &device_flag() { static bool f = false; return f; }
        inline bool device_ready() { return device_flag(); }
        inline void set_device_ready(bool v) { device_flag() = v; }

#if defined(KOKKOS_ENABLE_CUDA)
        using ExecSpace = Kokkos::Cuda;
        using MemSpace = Kokkos::CudaSpace;
#elif defined(KOKKOS_ENABLE_HIP)
        using ExecSpace = Kokkos::HIP;
        using MemSpace = Kokkos::HIPSpace;
#else
        using ExecSpace = Kokkos::DefaultExecutionSpace;
        using MemSpace = typename ExecSpace::memory_space;
#endif

        inline const char *execution_space_name()
        {
#if defined(KOKKOS_ENABLE_CUDA)
            return "Kokkos::Cuda";
#elif defined(KOKKOS_ENABLE_HIP)
            return "Kokkos::HIP";
#elif defined(KOKKOS_ENABLE_SYCL)
            return "Kokkos::SYCL";
#else
            return "Kokkos::DefaultExecutionSpace (CPU)";
#endif
        }
        using DComplex = Kokkos::complex<double>;
        using ComplexView = Kokkos::View<DComplex *, MemSpace>;
        using DoubleView = Kokkos::View<double *, MemSpace>;
        using MaskView = Kokkos::View<uint64_t *, MemSpace>;

        class StateVectorDevice
        {
        public:
            explicit StateVectorDevice(size_t dim = 0) { if (dim) resize(dim); }

            size_t size() const { return size_; }
            bool valid() const { return sv_.data() != nullptr; }
            void resize(size_t n)
            {
                if (n == size_ && valid()) return;
                sv_ = ComplexView("QuantumAccel::sv", n);
                host_ = Kokkos::create_mirror_view(sv_);
                size_ = n;
            }

            void upload(const std::complex<double> *host, size_t n)
            {
                resize(n);
                for (size_t i = 0; i < n; ++i)
                    host_(i) = DComplex(host[i].real(), host[i].imag());
                Kokkos::deep_copy(sv_, host_);
            }
            void download(std::complex<double> *host, size_t n) const
            {
                Kokkos::deep_copy(host_, sv_);
                for (size_t i = 0; i < n && i < size_; ++i)
                    host[i] = std::complex<double>(host_(i).real(), host_(i).imag());
            }
            ComplexView &view() { return sv_; }
            const ComplexView &view() const { return sv_; }

            void apply_ry(size_t q, double angle)
            {
                const double c = std::cos(angle * 0.5), s = std::sin(angle * 0.5);
                const uint64_t mask = uint64_t(1) << q;
                auto sv = sv_;
                Kokkos::parallel_for("QA::RY", Kokkos::RangePolicy<ExecSpace>(0, size_),
                    KOKKOS_LAMBDA(const uint64_t i) {
                        if (i & mask) return;
                        const uint64_t j = i | mask;
                        const DComplex a = sv(i), b = sv(j);
                        sv(i) = DComplex(c * a.real() - s * b.real(), c * a.imag() - s * b.imag());
                        sv(j) = DComplex(s * a.real() + c * b.real(), s * a.imag() + c * b.imag());
                    });
            }

            void apply_rz(size_t q, double angle)
            {
                const double cr = std::cos(angle), ci = std::sin(angle);
                const uint64_t mask = uint64_t(1) << q;
                auto sv = sv_;
                Kokkos::parallel_for("QA::RZ", Kokkos::RangePolicy<ExecSpace>(0, size_),
                    KOKKOS_LAMBDA(const uint64_t i) {
                        if (!(i & mask)) return;
                        const DComplex a = sv(i);
                        sv(i) = DComplex(a.real() * cr - a.imag() * ci,
                                         a.real() * ci + a.imag() * cr);
                    });
            }

            void apply_cnot(size_t control, size_t target)
            {
                const uint64_t cm = uint64_t(1) << control, tm = uint64_t(1) << target;
                auto sv = sv_;
                Kokkos::parallel_for("QA::CNOT", Kokkos::RangePolicy<ExecSpace>(0, size_),
                    KOKKOS_LAMBDA(const uint64_t i) {
                        if (!(i & cm) || (i & tm)) return;
                        const uint64_t j = i | tm;
                        const DComplex t = sv(i);
                        sv(i) = sv(j);
                        sv(j) = t;
                    });
            }

            std::vector<double> batch_expectation_z(const std::vector<size_t> &bits) const
            {
                const size_t D = bits.size();
                if (D == 0 || !valid()) return {};
                MaskView d_masks("QA::masks", D);
                auto h_masks = Kokkos::create_mirror_view(d_masks);
                for (size_t j = 0; j < D; ++j) h_masks(j) = uint64_t(1) << bits[j];
                Kokkos::deep_copy(d_masks, h_masks);

                Kokkos::View<double *, MemSpace> sums("QA::p1", D);
                Kokkos::deep_copy(sums, 0.0);
                const size_t block = 256;
                const size_t blocks = (size_ + block - 1) / block;
                auto sv = sv_;
                const size_t n = size_;
                Kokkos::parallel_for("QA::BatchExpectZ", Kokkos::RangePolicy<ExecSpace>(0, blocks),
                    KOKKOS_LAMBDA(const size_t b) {
                        const size_t begin = b * block, end = (begin + block < n) ? begin + block : n;
                        for (size_t i = begin; i < end; ++i)
                        {
                            const DComplex a = sv(i);
                            const double p = a.real() * a.real() + a.imag() * a.imag();
                            if (p == 0.0) continue;
                            for (size_t j = 0; j < D; ++j)
                                if (i & d_masks(j)) Kokkos::atomic_add(&sums(j), p);
                        }
                    });
                Kokkos::fence();
                auto h_sums = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), sums);
                std::vector<double> out(D);
                for (size_t j = 0; j < D; ++j) out[j] = 1.0 - 2.0 * h_sums(j);
                return out;
            }

            std::vector<size_t> sample_shots(size_t shots, uint64_t seed) const
            {
                if (!valid() || shots == 0) return {};
                DoubleView d_prob("QA::prob", size_);
                auto sv = sv_;
                Kokkos::parallel_for("QA::Prob", Kokkos::RangePolicy<ExecSpace>(0, size_),
                    KOKKOS_LAMBDA(const size_t i) {
                        const DComplex a = sv(i);
                        d_prob(i) = a.real() * a.real() + a.imag() * a.imag();
                    });
                Kokkos::fence();
                auto h_prob = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), d_prob);
                std::vector<double> cdf(size_);
                double acc = 0.0;
                for (size_t i = 0; i < size_; ++i) { acc += h_prob(i); cdf[i] = acc; }
                if (acc <= 0.0) return {};
                uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ULL;
                std::vector<size_t> out;
                out.reserve(shots);
                for (size_t k = 0; k < shots; ++k)
                {
                    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                    const double u = (double)(x >> 11) / 9007199254740992.0 * acc;
                    out.push_back(static_cast<size_t>(
                        std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin()));
                }
                return out;
            }

        private:
            ComplexView sv_;
            using HostView = typename ComplexView::host_mirror_type;
            mutable HostView host_;
            size_t size_ = 0;
        };

        struct Gate
        {
            enum class Type { RY, RZ, CNOT } type = Type::RY;
            size_t q0 = 0, q1 = 0;
            size_t param = 0;
            double scale = 1.0;
        };

        struct CircuitPlan
        {
            std::vector<Gate> gates;
            size_t original_count = 0, fused_count = 0, eliminated = 0, reordered = 0;
        };

        inline CircuitPlan fuse_gates(const std::vector<Gate> &input)
        {
            CircuitPlan plan;
            plan.original_count = input.size();
            std::vector<Gate> work = input;

            std::vector<Gate> noid;
            for (const auto &g : work)
            {
                if (g.scale == 0.0 && g.type != Gate::Type::CNOT) { ++plan.eliminated; continue; }
                noid.push_back(g);
            }
            work.swap(noid);

            std::vector<Gate> merged;
            for (const auto &g : work)
            {
                if (!merged.empty() && g.type == Gate::Type::RZ &&
                    merged.back().type == Gate::Type::RZ && merged.back().q0 == g.q0 &&
                    merged.back().param == g.param && merged.back().scale == g.scale)
                {
                    merged.back().scale *= 2.0;
                    ++plan.fused_count;
                    continue;
                }
                merged.push_back(g);
            }
            work.swap(merged);

            std::vector<Gate> reordered;
            std::vector<Gate> pending_rz;
            for (const auto &g : work)
            {
                if (g.type == Gate::Type::RZ) { pending_rz.push_back(g); continue; }
                bool commutes = true;
                for (const auto &r : pending_rz)
                    if (r.q0 == g.q0 || (g.type == Gate::Type::CNOT && r.q0 == g.q1))
                        commutes = false;
                if (!commutes)
                {
                    reordered.insert(reordered.end(), pending_rz.begin(), pending_rz.end());
                    pending_rz.clear();
                }
                reordered.push_back(g);
            }
            reordered.insert(reordered.end(), pending_rz.begin(), pending_rz.end());
            plan.reordered = static_cast<size_t>(std::max<ptrdiff_t>(0,
                static_cast<ptrdiff_t>(input.size()) - static_cast<ptrdiff_t>(reordered.size())));
            plan.gates = std::move(reordered);
            return plan;
        }

        struct QuantumAccelPolicy
        {
            bool fuse_gates = true;
            bool batch_expectation = true;
            bool shot_batching = true;
            bool use_device = true;
        };

        inline std::vector<double> qm_batch_expectation_z(
            const std::vector<std::complex<double>> &state,
            const std::vector<size_t> &qubits, bool use_device = true)
        {
            if (qubits.empty() || state.empty()) return {};
            if (use_device)
            {
                StateVectorDevice sv(state.size());
                sv.upload(state.data(), state.size());
                return sv.batch_expectation_z(qubits);
            }
            const size_t D = qubits.size();
            std::vector<uint64_t> masks(D);
            for (size_t j = 0; j < D; ++j) masks[j] = uint64_t(1) << qubits[j];
            std::vector<double> p1(D, 0.0);
            for (size_t i = 0; i < state.size(); ++i)
            {
                const double p = std::norm(state[i]);
                if (p == 0.0) continue;
                for (size_t j = 0; j < D; ++j)
                    if (i & masks[j]) p1[j] += p;
            }
            std::vector<double> out(D);
            for (size_t j = 0; j < D; ++j) out[j] = 1.0 - 2.0 * p1[j];
            return out;
        }

        inline std::vector<size_t> qm_sample_shots(
            const std::vector<std::complex<double>> &state, size_t shots,
            uint64_t seed = 0, bool use_device = true)
        {
            if (state.empty() || shots == 0) return {};
            if (use_device)
            {
                StateVectorDevice sv(state.size());
                sv.upload(state.data(), state.size());
                return sv.sample_shots(shots, seed);
            }
            std::vector<double> cdf(state.size());
            double acc = 0.0;
            for (size_t i = 0; i < state.size(); ++i) { acc += std::norm(state[i]); cdf[i] = acc; }
            uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ULL;
            std::vector<size_t> out;
            out.reserve(shots);
            for (size_t k = 0; k < shots; ++k)
            {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                const double u = (double)(x >> 11) / 9007199254740992.0 * acc;
                out.push_back(static_cast<size_t>(
                    std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin()));
            }
            return out;
        }

        inline std::string describe(const DeviceInfo &d)
        {
            return std::string("vendor=") + vendor_name(d.vendor) +
                   " backend=" + backend_name(d.backend) +
                   " devices=" + std::to_string(d.count) +
                   " name=" + (d.name.empty() ? "-" : d.name) +
                   " usable=" + (d.usable ? "yes" : "no") +
                   " reason=" + d.reason;
        }
    }
}