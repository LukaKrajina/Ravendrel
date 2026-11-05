#pragma once
// ─── 全局 qubit id 分配器（src/QObject.hpp 与 qml/Inference.hpp 共用）───
// 修复原先「纯单调计数 + rvdl_release_object 盲目 fetch_sub」的分配器缺陷：
//   1. fetch_sub(size) 在释放非尾部 id 时会让下一个分配的 id 与仍存活的 id 冲突；
//   2. __quantum__rt__qubit_release 未做任何回收，计数与 QObject 层不一致。
// 现在分配优先复用空闲 id，释放 id 回收到空闲栈，next_available_qubit 仅作高水位。
//
// 独立成头的原因：Inference.hpp 与 src/QObject.hpp 曾互相包含（QObject 需要分配器、
// Inference 需要 QObject 结构）。分配器拆出后，无论哪个头先被包含都能独立编译
// （此前先包含 Inference.hpp 的 TU 会因 QObject.hpp 引用的分配器尚未声明而编译失败）。

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

// 分配一个 qubit id：优先复用已释放的空闲 id，否则递增全局高水位。
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

// 释放一个 qubit id 到空闲池（去重，避免重复回收同一 id）。
inline void rvdl_free_qubit_id(size_t id)
{
    std::lock_guard<std::mutex> lock(qubit_allocator_mutex());
    auto &free = qubit_free_list();
    if (std::find(free.begin(), free.end(), id) == free.end())
        free.push_back(id);
}
