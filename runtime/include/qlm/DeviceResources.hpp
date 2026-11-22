#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif

namespace qlm
{
    namespace training
    {
        inline constexpr size_t kMinTrainingQubits = 8;

        inline constexpr size_t kMaxTrainingQubits = 20;

        inline size_t available_memory_bytes()
        {
#if defined(_WIN32)
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (GlobalMemoryStatusEx(&status))
                return static_cast<size_t>(status.ullAvailPhys);
            return 0;
#elif defined(__APPLE__)
            mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
            vm_statistics64_data_t vmstat{};
            if (host_statistics64(mach_host_self(), HOST_VM_INFO,
                                  reinterpret_cast<host_info64_t>(&vmstat),
                                  &count) == KERN_SUCCESS)
            {
                const size_t page = static_cast<size_t>(vm_page_size);
                return (static_cast<size_t>(vmstat.free_count) +
                        static_cast<size_t>(vmstat.inactive_count)) *
                       page;
            }
            return 0;
#elif defined(__linux__) || defined(__ANDROID__)
            const long pages = sysconf(_SC_AVPHYS_PAGES);
            const long page_size = sysconf(_SC_PAGE_SIZE);
            if (pages > 0 && page_size > 0)
                return static_cast<size_t>(pages) * static_cast<size_t>(page_size);
            return 0;
#else
            return 0;
#endif
        }

        inline size_t max_training_qubits()
        {
            const size_t avail = available_memory_bytes();
            if (avail == 0)
                return kMaxTrainingQubits;

            const size_t budget = avail / 2;
            const size_t per_amp = sizeof(std::complex<double>);
            size_t best = kMinTrainingQubits;
            for (size_t q = kMinTrainingQubits; q <= kMaxTrainingQubits; ++q)
            {
                const size_t bytes = (size_t(1) << q) * per_amp * 4;
                if (bytes > budget)
                    break;
                best = q;
            }
            return best;
        }
    }
}