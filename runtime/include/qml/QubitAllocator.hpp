#pragma once

#include <atomic>
#include <cstddef>
#include <vector>
#include <mutex>
#include <algorithm>

inline std::atomic<size_t> next_available_qubit{0};

inline std::vector<size_t> &qubit_free_list()
{
    static std::vector<size_t> list;
    return list;
}
inline std::mutex &qubit_allocator_mutex()
{
    static std::mutex m;
    return m;
}

inline size_t rvdl_alloc_qubit_id()
{
    std::lock_guard<std::mutex> lock(qubit_allocator_mutex());
    auto &free = qubit_free_list();
    if (!free.empty())
    {
        size_t id = free.back();
        free.pop_back();
        return id;
    }
    return next_available_qubit.fetch_add(1);
}

inline void rvdl_free_qubit_id(size_t id)
{
    std::lock_guard<std::mutex> lock(qubit_allocator_mutex());
    auto &free = qubit_free_list();
    if (std::find(free.begin(), free.end(), id) == free.end())
        free.push_back(id);
}