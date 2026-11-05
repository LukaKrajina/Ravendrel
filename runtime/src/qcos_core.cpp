// ============================================================================
//  统一翻译单元
//
//  仅用于验证 freestanding 头文件可独立编译（-ffreestanding -fno-exceptions
//  -fno-rtti，无 libstdc++）。真正的内核逻辑分布在 include/qcos/*.hpp 中。
// ============================================================================

#include "qcos/Arch.hpp"
#include "qcos/LibcSubset.hpp"
#include "qcos/Spinlock.hpp"
#include "qcos/Pmm.hpp"
#include "qcos/DragonPmm.hpp"
#include "qcos/Sched.hpp"
#include "qcos/Vmm.hpp"
#include "qcos/Vmm_x86_64.hpp"
#include "qcos/Vmm_aarch64.hpp"
#include "qcos/Qms.hpp"
