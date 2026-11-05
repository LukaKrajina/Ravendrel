cmake_minimum_required(VERSION 3.10)
project(runtime VERSION 0.6.5 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 注：clang++（MSVC 目标）在 Windows 下编译 EXE 时，若依赖库的 CRT 运行时不一致
# （Kokkos 为静态 /MT、vendor GLFW 预编译库为动态 /MD），会导致 UCRT 符号链接失败。
# 需统一依赖库的 CRT 运行时（重新编译 Kokkos 或 GLFW）后，EXE 才能链接。
# 核心库（rvdl_rt / qcos_core）不依赖 GLFW，可正常编译。

# ─── 构建选项 ──────────────────────────────────
# Sanitizer:ASan + UBSan,用于 CI/本地排查内存与未定义行为
option(RVDLRSP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(RVDLRSP_SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    message(STATUS "Sanitizers (ASan/UBSan) ENABLED")
endif()

# 代码覆盖率(gcov/lcov):RVDLRSP_COVERAGE=ON 时用 --coverage + O0 便于行覆盖
option(RVDLRSP_COVERAGE "Enable code coverage (gcov/lcov)" OFF)
if(RVDLRSP_COVERAGE)
    if(MSVC)
        message(WARNING "MSVC coverage requires /profile; use clang/gcc --coverage instead")
    else()
        add_compile_options(--coverage -O0 -g)
        add_link_options(--coverage)
    endif()
    message(STATUS "Code coverage (gcov/lcov) ENABLED")
endif()

# 基础编译告警(-Werror 待告警清零后启用)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra)
endif()

add_compile_definitions(NOMINMAX)
add_compile_definitions(_USE_MATH_DEFINES)
add_compile_definitions(NDEBUG)

# NOTE(ravendrel): 原先用 if(MSVC) 判断，但 clang++（GNU 风格命令行）在 Windows 上
# MSVC 变量为假，会误入 else() 分支加入 -fPIC —— 而 clang++ 以
# x86_64-pc-windows-msvc 为目标时会拒绝 -fPIC（"unsupported option"）。
# 改为按平台判断：Windows 上只加安全宏，/EHsc 仅对真 MSVC 生效。
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        add_compile_options(/EHsc)
    endif()
else()
    add_compile_options(-fPIC)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ==========================================
# qcos_core：freestanding 内核核心
#   无 LLVM / 无 Kokkos / 无 libstdc++，仅依赖编译器内置。
#   -ffreestanding 关掉宿主 libc 假设；-fno-exceptions/-fno-rtti 是内核惯例。
#   已就绪：DragonPmm（驯龙系统）、Vmm（x86_64/aarch64 双后端）、Idt/Irq 中断、
#   Timer 级联时间轮、PreemptSched（E²VDF 抢占式调度）、Serial/Qms/Spinlock/Sched。
# ==========================================
add_library(qcos_core STATIC
    src/qcos_core.cpp
)
target_include_directories(qcos_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(qcos_core PUBLIC cxx_std_20)
set_target_properties(qcos_core PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(qcos_core PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -Wall -Wextra
)

# qcos_smoke：宿主机冒烟测试（hosted，用 <cstdio>/<cassert> 校验 freestanding
# 头文件的逻辑）。不在 QEMU 里跑，纯粹验证 Pmm/Sched/LibcSubset/Spinlock 正确。
add_executable(qcos_smoke
    src/qcos_smoke.cpp
)
target_include_directories(qcos_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(qcos_smoke PRIVATE cxx_std_20)

# qcos 为 freestanding 内核目标，必须用宿主编译器（g++）而非 nvcc 编译：
# nvcc 会注入 cuda_runtime.h，在 -ffreestanding 下触发 requires_hosted.h，
# 且其引入的 <cstring> 会与 qcos::memset/memcpy/strlen/memcmp 重载冲突。
if(CMAKE_CXX_COMPILER MATCHES "nvcc_wrapper")
    target_compile_options(qcos_core PRIVATE --host-only)
    target_compile_options(qcos_smoke PRIVATE --host-only)
endif()

# qchain_selftest：量子区块链核心自测（header-only，不依赖 Kokkos/LLVM）。
# 覆盖 SHA3/HMAC/HKDF、ML-KEM-768、LweKem、Csprng、UnicodeHash256、
# CausalityGuard、SpacetimeCipher、UTXO 双花检测与签名自包含。
add_executable(qchain_selftest
    src/qchain_selftest.cpp
)
target_include_directories(qchain_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(qchain_selftest PRIVATE cxx_std_20)

# ==========================================
# 1. 依赖
# ==========================================

# LLVM
find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

if(WIN32)
    # NOTE(ravendrel): 原为 `native`（仅宿主后端）。但 Compiler/JIT 会调用 LLVM 的
    # InitializeAllTargetInfos() / InitializeAllTargets() / InitializeAllTargetMCs()
    # 系列宏，它们引用**全部**后端入口（AArch64/ARM/BPF/WebAssembly/RISCV/NVPTX...），
    # 只链 native 会导致 lld-link 大量 "undefined symbol: LLVMInitialize<T>Target*"。
    # 正确做法是用 LLVMConfig 导出的 LLVM_TARGETS_TO_BUILD（该 LLVM 实际构建的
    # 后端列表），恰好覆盖 InitializeAll* 系列宏所引用的入口；直接写 "all-targets"
    # 会被当成字面组件名，生成不存在的 LLVMall-targets.lib。
    llvm_map_components_to_libnames(llvm_libs
        Core
        ExecutionEngine
        IRReader
        OrcJIT
        Support
        Target
        ${LLVM_TARGETS_TO_BUILD}
    )
else()
    # Linux：优先**静态**组件库——产物自包含，不要求目标机装 libLLVM.so（既省去
    # 132 MB 依赖，也避免跨发行版的 LLVM 版本/符号耦合）。Ubuntu/Debian 的
    # llvm-dev 同时提供 libLLVM*.a 与合并的 libLLVM.so；Arch 等只有合并 so。
    find_library(RVDL_LLVM_STATIC_CORE NAMES LLVMCore PATHS ${LLVM_LIBRARY_DIR} NO_DEFAULT_PATH)
    if(RVDL_LLVM_STATIC_CORE)
        llvm_map_components_to_libnames(llvm_libs
            Core
            ExecutionEngine
            IRReader
            OrcJIT
            Support
            Target
            ${LLVM_TARGETS_TO_BUILD}
        )
        # 静态链接还需 LLVM 自身的外部依赖（zlib / zstd / tinfo / xml2 / dl / m …）
        list(APPEND llvm_libs ${LLVM_SYSTEM_LIBS})
        message(STATUS "Ravendrel: LLVM 静态链接 (${RVDL_LLVM_STATIC_CORE})，系统库: ${LLVM_SYSTEM_LIBS}")
    else()
        set(llvm_libs LLVM)
        message(STATUS "Ravendrel: LLVM 以合并 libLLVM.so 动态链接")
    endif()
endif()

# Vulkan 与 GLFW
find_package(Vulkan REQUIRED)
include_directories(${Vulkan_INCLUDE_DIRS})
set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor")
include_directories(${VENDOR_DIR})
include_directories(${VENDOR_DIR}/GLFW/)
include_directories(${VENDOR_DIR}/Nuklear/)

# GLFW：Windows 使用 vendor 预编译库，Linux 使用系统 libglfw（Ubuntu 为 libglfw.so）。
if(WIN32)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # MinGW 交叉编译：vendor 的 mingw-w64 预编译库（静态 libglfw3.a）
        link_directories(${VENDOR_DIR}/GLFW/lib-mingw-w64)
        set(GLFW_STATIC_SYS gdi32 user32 shell32 dwmapi winmm shcore)
    else()
        # MSVC：vendor 的 lib-vc2026
        link_directories(${VENDOR_DIR}/GLFW/lib-vc2026)
        set(GLFW_STATIC_SYS "")
    endif()
    set(GLFW_LIB glfw3)
else()
    set(GLFW_LIB glfw)
    set(GLFW_STATIC_SYS "")
endif()

# Kokkos：Windows 默认从 C:/Libraries/kokkos 查找；Linux 通过 vcpkg 或
# -DKokkos_DIR=<path> 提供。可通过 -DKokkos_ROOT / -DKokkos_DIR 覆盖。
if(WIN32 AND NOT Kokkos_DIR)
    unset(Kokkos_DIR CACHE)
    set(Kokkos_DIR "C:/Libraries/kokkos/lib/cmake/Kokkos")
endif()
# KokkosConfig.cmake 默认会让所有 C++ 编译命令改走 kokkos_launch_compiler，
# 但那是 bash 脚本，Windows 下 ninja CreateProcess 直接失败
# ("%1 is not a valid Win32 application")。Clang 的 CUDA 由 Kokkos 导出的
# INTERFACE_COMPILE_OPTIONS（-x cuda / --cuda-gpu-arch=...）提供，无需重定向。
set(Kokkos_LAUNCH_COMPILER OFF)
find_package(Kokkos REQUIRED)
# CRT 运行时（/MD 动态）通过 build.bat 的 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
# 传入（必须在 project() 之前，命令行参数方式最可靠），此处不再重复设置。
# 需与 LLVM（C:\Libraries\LLVM-md）、vendor/GLFW(vc2026)、Qt6 的 /MD 保持一致。

# Ravendrel: 手动 OpenMP（clang-cl 用 -openmp + libomp，绕过 FindOpenMP 对 clang-cl 的缺陷）
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-openmp)
    link_directories("C:/Program Files/LLVM/lib")
    set(OPENMP_LIB libomp)
elseif(MSVC)
    # MSVC 用 /openmp:llvm（LLVM OpenMP 运行时，支持 OpenMP 3.0+，含 omp_get_level）
    add_compile_options(/openmp:llvm)
    link_directories("C:/Libraries/LLVM/lib")
    set(OPENMP_LIB libomp)
endif()
set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "")
set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK "")

# ==========================================
# 国产化加速适配：龙芯 LoongArch CPU + 摩尔线程 MUSA GPU
# ==========================================
# 龙芯 CPU：编译器自动定义 __loongarch__，此处仅做识别与提示；
# 编译期架构分支由 qcos/Arch.hpp 与 qhal/GpuBackend.hpp 处理。
if(CMAKE_SYSTEM_PROCESSOR MATCHES "loongarch|loongson")
    message(STATUS "Target CPU: LoongArch (龙芯)")
    add_compile_definitions(RVDL_CPU_LOONGARCH=1)
endif()

# 摩尔线程 MUSA GPU：类 CUDA 编程模型（mcc 编译器 + musa 运行时）。
# 通过 -DRVDL_GPU_BACKEND=MUSA 显式启用，或设置 MUSA_HOME 后 AUTO 自动探测。
set(RVDL_GPU_BACKEND "AUTO" CACHE STRING "GPU backend: AUTO/CUDA/MUSA/HIP/OFF")
if(RVDL_GPU_BACKEND STREQUAL "MUSA" OR
   (RVDL_GPU_BACKEND STREQUAL "AUTO" AND (DEFINED ENV{MUSA_HOME} OR DEFINED ENV{MTHREADS_HOME})))
    find_program(MCC_COMPILER mcc
        HINTS "$ENV{MUSA_HOME}/bin" "$ENV{MTHREADS_HOME}/bin")
    if(MCC_COMPILER)
        message(STATUS "摩尔线程 MUSA GPU detected: ${MCC_COMPILER}")
        add_compile_definitions(RVDL_GPU_MUSA_BUILD=1)
    else()
        message(STATUS "RVDL_GPU_BACKEND=MUSA 但未找到 mcc 编译器，回退 CPU 后端")
    endif()
endif()

# 全局 include 路径
include_directories(include)

# ─── 代码签名辅助函数（WDAC 拦截 → 构建后自动签名）──────────
function(rvdl_sign_target TARGET_NAME)
    if(WIN32)
        find_program(POWERSHELL_EXE powershell)
        if(POWERSHELL_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sign.ps1")
            add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${POWERSHELL_EXE} -ExecutionPolicy Bypass -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/sign.ps1"
                    -Targets "$<TARGET_FILE:${TARGET_NAME}>"
                COMMENT "Code signing ${TARGET_NAME}.exe")
        endif()
    endif()
endfunction()

# ==========================================
# 2. TARGET: RVDL_RT_HOST 主机专用静态库（温控/WMI/串口）
#     含 Windows COM(SEH)/串口代码，不能被 Kokkos 的 CUDA 编译规则处理，
#     故单独成库且不链接 Kokkos。
# ==========================================
add_library(rvdl_rt_host STATIC
    src/ThermalSimulation.cpp
    src/ThermalController.cpp
)
target_include_directories(rvdl_rt_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    # wbemuuid/ole32/oleaut32: WMI 温度读取（COM）
    target_link_libraries(rvdl_rt_host PRIVATE wbemuuid ole32 oleaut32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(rvdl_rt_host PRIVATE pthread m)
endif()

# ==========================================
# 3. TARGET: RVDL_RT 共享库（量子核心）
#     包含LLVM + Kokkos，产出 librvdl_rt.dll / librvdl_rt.so
# ==========================================
add_library(rvdl_rt SHARED
    src/runtime_api.cpp
    src/AtomicVaporORAM.cpp
    src/ClassicGfx.cpp
    src/ClassicGui.cpp
    src/BroUi.cpp         # Bro 桌面端 UI 原语（绝对定位文本 / 键盘 / 滚轮）
    src/BroStr.cpp        # Bro 可变字符串（rvdl 侧可写文本缓冲）
    src/BroSyntax.cpp     # Bro 行索引 + 词法着色（编辑器视图）
    src/qicon.cpp         # Bro 图标库（SDF 程序化光栅化）
    src/QuantumGfx.cpp   # 量子图形引擎（PEQG）：状态内省 / 谓词 / 复值图元
    src/QuantumUi.cpp    # 量子 UI 库（PCQW）：谓词耦合的量子控件
    src/stb_image_impl.cpp
    src/glibc_compat.cpp   # 仅 Linux/glibc 生效：见文件头（旧发行版符号版本兼容）
    include/gui/src/render_context.cpp
    include/gui/src/font_loader.cpp
)
target_compile_definitions(rvdl_rt PRIVATE RVDL_RT_BUILD)
target_include_directories(rvdl_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# ClassicGfx 复用 rvdlRSP 的 VulkanContext；ClassicGui 复用 gui/src 的 RenderContext
target_include_directories(rvdl_rt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../RSP/include)
target_include_directories(rvdl_rt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/src)
target_link_libraries(rvdl_rt PRIVATE rvdl_rt_host ${llvm_libs})
target_link_libraries(rvdl_rt PRIVATE Kokkos::kokkos)
# Kokkos（CUDA 后端）为动态库 libkokkoscore.so：默认 --as-needed 会在
# shared library 链接时丢弃其符号，导致 rvdl_rt.so 遗留未解析的 Kokkos
# 符号、runtime 可执行链接失败。强制 --no-as-needed 保留 Kokkos 依赖。
target_link_options(rvdl_rt PRIVATE -Wl,--no-as-needed)
if(UNIX AND NOT APPLE)
    # glibc 兼容垫片必须关掉内建函数折叠，否则我们对 fmod/sqrtf 的「就地定义」
    # 会被编译器替换成内建调用，垫片形同虚设。
    set_source_files_properties(src/glibc_compat.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")
    # 静态链接 libstdc++：产物不再要求目标机的 libstdc++.so.6 具备同版本
    # GLIBCXX_3.4.3x 符号（Ubuntu 22.04 只到 3.4.30）。
    # 注：**不**静态链接 libgcc——静态 libgcc 的展开器会引用 _dl_find_object
    # （ld.so 自 glibc 2.35 起才导出），把门槛又抬上去；libgcc_s.so.1 各发行版
    # 都有且版本接口稳定，动态链接反而更通用。
    target_link_options(rvdl_rt PRIVATE -static-libstdc++)
endif()
target_link_libraries(rvdl_rt PRIVATE ${Vulkan_LIBRARIES} ${GLFW_LIB} ${GLFW_STATIC_SYS})
if(WIN32)
    # msvcprt.lib：MSVC STL 的「算法内联化」内部实现（__std_rotate / __std_unique_N /
    #   __std_min_element_N / __std_find_* / __std_regex_transform_primary_char 等）。
    #   预编译的 LLVM-md 静态库是用新版 MSVC 编的，会引用这些符号；而本工程走
    #   /MD（MultiThreadedDLL），动态 CRT 不会把它们带进来 —— 直接链接必然报
    #   undefined symbol。补上 msvcprt.lib 即可。
    #   注意：这些是纯算法/查表函数，不碰堆与 FILE*，与 /MD 混用是安全的。
    target_link_libraries(rvdl_rt PRIVATE msvcprt.lib)
    target_link_libraries(rvdl_rt PRIVATE ws2_32 ${OPENMP_LIB})
    # Fox C ABI 导出（桌面宿主用 MinGW g++ 链接 -lrvdl_rt）。
    # fox_core 是静态库、fox.h 没有 dllexport，不显式导出的话宿主链接期
    # 报 undefined reference to fox_create 等。见 rvdl_rt_fox.def。
    target_link_options(rvdl_rt PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/rvdl_rt_fox.def")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(rvdl_rt PRIVATE pthread dl m)
endif()

# ==========================================
# 3.5 TARGET: MIR_MODULE_TEST
#     MirModuleBuilder 下沉测试（MIR JSON → LLVM Module → verifyModule）。
#     仅依赖 LLVM（Core/Support），不依赖 Kokkos/Vulkan/GLFW。
# ==========================================
add_executable(mir_module_test
    src/mir_module_test.cpp
)
target_include_directories(mir_module_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mir_module_test PRIVATE cxx_std_20)
target_link_libraries(mir_module_test PRIVATE ${llvm_libs})
if(WIN32)
    target_link_libraries(mir_module_test PRIVATE ws2_32 psapi shell32 ole32 uuid advapi32 ntdll)
endif()

# ==========================================
# 4. TARGET: RUNTIME 壳子
#    仅含 socket daemon + stdin 交互，通过 C ABI 调用 rvdl_rt
# ==========================================
add_executable(runtime
    src/main.cpp
    src/glibc_compat.cpp   # 仅 Linux/glibc 生效（同上）
)
target_include_directories(runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# rvdl_rt 对 ${GLFW_LIB}/${Vulkan_LIBRARIES} 是 PRIVATE 链接，符号不会传递过来；
# 而 librvdl_rt.so 里仍留有 glfw* 的未定义引用，链接可执行文件时必须显式补上
# （否则 --no-allow-shlib-undefined 报 "undefined reference to glfwInit"）。
target_link_libraries(runtime PRIVATE rvdl_rt Kokkos::kokkos ${GLFW_LIB} ${Vulkan_LIBRARIES})
# rvdl_rt.so（shared）遗留未解析的 Kokkos 符号（CUDA 设备符号在 shared 链接下
# 不导出），runtime 可执行链接时需显式带上 Kokkos 以解析这些符号。
target_link_options(runtime PRIVATE -Wl,--no-as-needed)
if(WIN32)
    # 应用图标（runtime.ico → 嵌入 runtime.exe）
    target_sources(runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../icons/runtime.rc")
    target_link_libraries(runtime PRIVATE ws2_32 advapi32)
    # 无控制台窗口（后台服务 / 由管道驱动）
    set_target_properties(runtime PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # subsystem:windows 默认找 WinMain，显式指定 main 入口（clang++/MSVC 均需）
        target_link_options(runtime PRIVATE -Xlinker /ENTRY:mainCRTStartup)
    else()
        # MinGW：-mwindows 默认找 WinMain，显式指定 mainCRTStartup 入口
        target_link_options(runtime PRIVATE -Wl,--entry=mainCRTStartup)
    endif()
elseif(UNIX AND NOT APPLE)
    target_link_libraries(runtime PRIVATE pthread)
    # 与 rvdl_rt 一致：静态 libstdc++（libgcc 保持动态，见上）
    target_link_options(runtime PRIVATE -static-libstdc++)
endif()
rvdl_sign_target(runtime)

# ==========================================
# train_cli：Jarvis 命令行训练器（QVM 后端 + QLM + qqnt tokenizer + LF2C2）。
# 复用 runtime 的训练链路，去掉 GUI，供脚本化训练 manifest/*/train.txt。
# 依赖 rvdl_rt（QVM/QLM/tokenizer 符号）+ Kokkos（QVM 的 GPU 后端）。
# ==========================================
add_executable(train_cli
    tools/train_cli.cpp
)
target_include_directories(train_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(train_cli PRIVATE rvdl_rt Kokkos::kokkos ${GLFW_LIB} ${Vulkan_LIBRARIES})
target_link_options(train_cli PRIVATE -Wl,--no-as-needed)
if(UNIX AND NOT APPLE)
    target_link_libraries(train_cli PRIVATE pthread)
endif()

# QVM 策略路由验证（方案2 MPS 路由 + 方案1 device 批处理的策略选择自检）
add_executable(qvm_policy_test
    tools/qvm_policy_test.cpp
)
target_include_directories(qvm_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qvm_policy_test PRIVATE rvdl_rt Kokkos::kokkos ${GLFW_LIB} ${Vulkan_LIBRARIES})
target_link_options(qvm_policy_test PRIVATE -Wl,--no-as-needed)
if(UNIX AND NOT APPLE)
    target_link_libraries(qvm_policy_test PRIVATE pthread)
endif()

# ==========================================
# 5. TARGET: QLM TRANSMITTER
# ==========================================
# GUI 层用 Fox（窗口 / VK+CPU 双后端 / 输入 / 字体 / 主题 / 控件），
# 不再自己持有一套原始 Vulkan + Nuklear 的 GLFW 后端 —— fox_core 把那些
# 都收走了，这里只需要 i18n（界面文案的 tr()）。
add_executable(transmitter
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qml/Transmitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/i18n.cpp
)
target_include_directories(transmitter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Transmitter.cpp 用到 qhal/QVM.hpp 与 qlm/QLM.hpp —— 它们拉进 Ga.hpp，而
# Ga.hpp 需要 Kokkos 头；QVM/QLM 的实现又都在 librvdl_rt.so 里。
#
# rvdl_rt 是 **PRIVATE** 链 Kokkos 的（见上面的 target_link_libraries(rvdl_rt
# PRIVATE Kokkos::kokkos)），所以 Kokkos 的 include 目录**不会**传播给消费者
# —— 只链 fox_core 的 transmitter 于是拿不到 Kokkos_Core.hpp，编译直接失败：
#   fatal error: Kokkos_Core.hpp: No such file or directory
#
# 这里按 runtime 目标（能构建的那个）的方式补齐：rvdl_rt + Kokkos + GLFW +
# Vulkan，并同样加 --no-as-needed（librvdl_rt.so 留有未解析的 Kokkos 符号）。
target_link_libraries(transmitter PRIVATE fox_core rvdl_rt Kokkos::kokkos ${GLFW_LIB} ${Vulkan_LIBRARIES})
target_link_options(transmitter PRIVATE -Wl,--no-as-needed)
if(WIN32)
    target_link_libraries(transmitter PRIVATE ws2_32 gdi32 user32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(transmitter PRIVATE m pthread dl)
endif()

if (MSVC)
    set_target_properties(transmitter PROPERTIES ENABLE_EXPORTS TRUE)
endif()
if(WIN32)
    # GUI 程序：无控制台窗口
    set_target_properties(transmitter PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(transmitter PRIVATE -Xlinker /ENTRY:mainCRTStartup)
endif()
rvdl_sign_target(transmitter)

# ==========================================
# 6. TARGET: QVM VISUALIZER
# ==========================================
add_executable(qvm_visualizer 
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/src/main_gui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/src/render_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/src/app.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/src/daemon_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/src/font_loader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/components/circuit_grid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/gui/i18n.cpp
)
target_include_directories(qvm_visualizer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qvm_visualizer PRIVATE ${Vulkan_LIBRARIES} ${GLFW_LIB})

if(WIN32)
    target_link_libraries(qvm_visualizer PRIVATE ws2_32 gdi32)
    # GUI 程序：无控制台窗口
    set_target_properties(qvm_visualizer PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(qvm_visualizer PRIVATE -Xlinker /ENTRY:mainCRTStartup)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(qvm_visualizer PRIVATE m pthread dl)
endif()
rvdl_sign_target(qvm_visualizer)

# ==========================================
# 6.5 TARGET: FOX  (自研图形用户界面库 / 框架)
#
#   Fox 是为取代 Nuklear 而自研的 GUI 框架，风格对标 macOS 27 (Golden Gate)。
#   与上面几个目标不同，Fox **不链接 Nuklear**：
#     · TrueType 光栅化 (detail/stbtt_impl.hpp) 是从 Nuklear 里提取出来的私有副本，
#       这样「删掉 Nuklear 之后 Fox 仍能构建」这条要求是结构性成立的，而不是靠约定。
#     · 渲染走自己的 Vulkan SDF 管线 (backend/vk_backend.cpp + shaders.{vert,frag})，
#       Nuklear 的 nk_draw 只能画矩形和字形，做不出圆角 / 玻璃 / 阴影。
#
#   FOX_SHADERS_SPV_DIR 可指向预编译的 SPIR-V 目录；留空则使用
#   backend/shaders_spv.h（由 backend/embed_spv.ps1 生成，已入库）。
# ==========================================
set(FOX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ciUd/fox)

add_library(fox_core STATIC
    ${FOX_DIR}/theme/theme.cpp
    ${FOX_DIR}/text/font.cpp
    ${FOX_DIR}/input/input.cpp
    ${FOX_DIR}/layout/layout.cpp
    # TextBuffer：编辑模型（光标、选区、撤销），与 text_field 的控件代码分开：
    # 选区和撤销不是挂在 char* 上的附加功能，而是同一对象的两个视图。
    ${FOX_DIR}/text/text_buffer.cpp
    ${FOX_DIR}/core/context.cpp
    # Fox 自己提供不了的宿主服务。刻意不用 GLFW 的剪贴板：
    # glfwGetClipboardString 需要 glfwInit 和窗口上下文，而 Fox 必须在完全没有
    # 窗口时也能工作（headless、CI、QuarkOS）。
    ${FOX_DIR}/core/platform.cpp
    ${FOX_DIR}/core/platform_win32.cpp
    ${FOX_DIR}/widget/primitives.cpp
    # 组合控件（菜单栏/下拉、组合框、可选中项、可滚动分组、数值属性）。
    # 规则与原子控件相同：只用图元 + 显示列表构建，不碰任何后端。
    ${FOX_DIR}/widget/composite.cpp
    ${FOX_DIR}/window/window.cpp
    # PNG 解码 + mip 金字塔 + 采样（fox 自带 inflate，不用 stb_image）。
    # 无条件构建：fox 必须只靠 C++ 编译器就能构建；只在找到依赖时才存在的纹理
    # 路径，会在没人构建它的平台上烂掉。
    ${FOX_DIR}/image/png.cpp
    # 显示列表 → 实例，两个后端共用。从 VkBackend::build_instances 抽出来，让
    # CPU 光栅化器消费完全相同的几何。两份拷贝会漂移，漂移看起来像"CPU 渲染器
    # 有小毛病"，而不是"改了一处、漏了另一处"。
    ${FOX_DIR}/draw/instance.cpp
    # 后端放在库里而不是 demo 里：fox.cpp（C ABI）需要它，而且不能渲染的"核心"
    # 宿主根本没法用。
    ${FOX_DIR}/backend/vk_backend.cpp
    # 软件光栅化器，输出线性 RGBA8 缓冲。这是 freestanding 内核真正跑得动的
    # 后端（QuarkOS 没有 GPU、没有显示驱动、没有 libc），在任何地方都无需窗口
    # 和驱动即可运行。
    ${FOX_DIR}/backend/cpu_backend.cpp
    # C ABI，实现在上面的 C++ API 之上。
    ${FOX_DIR}/fox.cpp
)
target_include_directories(fox_core PUBLIC ${FOX_DIR})
# utils/Pa.hpp（像素加速器）刻意放在 fox 目录树之外：它是项目的，不是 Fox 的。
# Fox 通过这条 include 路径使用它，cpu_backend.cpp 可以去掉它而不必改其他
# Fox 文件。
target_include_directories(fox_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fox_core PUBLIC ${Vulkan_LIBRARIES} ${GLFW_LIB} ${GLFW_STATIC_SYS})
if(UNIX AND NOT APPLE)
    target_link_libraries(fox_core PUBLIC pthread m dl)
endif()

# rvdl_rt 的 cgui_*/cgfx_* 现在优先走 fox（运行时回退 Nuklear，见
# include/qhal/FoxHost.hpp），所以它必须链接 fox 的 C ABI。
# 放在 fox_core 定义之后：向前引用目标名虽然合法，但链接依赖写在定义旁边
# 才能一眼看出"谁依赖谁"。
target_link_libraries(rvdl_rt PRIVATE fox_core)

# fox_core 自带一份 stb_truetype 实现（detail/stbtt_impl.hpp，带度量修正），而
# Nuklear 的 NK_IMPLEMENTATION 编译单元（render_context.cpp）默认也会内联一份，
# 两份 stbtt_* 强符号在链接 rvdl_rt 时冲突（multiple definition）。
# 按 Nuklear 官方开关关掉本目标的内联实现：声明照旧可用，stbtt_* 在链接期
# 解析到 fox 的实现——保留的唯一一份正是带修正的副本。
# 注意必须是 target 级而非全局：qvm_visualizer 等不链接 fox 的目标仍依赖
# Nuklear 自带实现。
target_compile_definitions(rvdl_rt PRIVATE NK_NO_STB_TRUETYPE_IMPLEMENTATION)

add_executable(fox_demo
    ${FOX_DIR}/fox_demo.cpp
)
target_include_directories(fox_demo PRIVATE ${FOX_DIR})
target_link_libraries(fox_demo PRIVATE fox_core)
if(WIN32)
    target_link_libraries(fox_demo PRIVATE ws2_32 gdi32 user32)
    set_target_properties(fox_demo PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# C ABI 冒烟测试，按 C 语言编译。
#
# 这个目标的作用：fox.h 一旦不再是纯 C 头，就让构建失败。C++ 测试会容忍引用、
# 默认参数、命名空间，所以"C 头能编译"必须真的用 C 编译来证明。
add_executable(fox_c_test ${FOX_DIR}/fox_c_test.c)
target_link_libraries(fox_c_test PRIVATE fox_core)
set_target_properties(fox_c_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(WIN32)
    target_link_libraries(fox_c_test PRIVATE ws2_32 gdi32 user32)
endif()

# 无头渲染测试：无窗口、无 GPU、无驱动。
#
# 它不是又一个 demo，存在有两个理由。其一，它是唯一能在没有 Vulkan 的地方跑的
# Fox 二进制（freestanding 内核、CI 容器、驱动损坏的机器）。其二，它是参考实现
# 对照：它消费与 GPU 路径相同的实例流，同一 UI 走两条路渲染再对比，能真正检验
# shaders.frag。窗口截图做不到：可能被遮挡、在调整大小中途截取，或显示过期的
# 合成器表面——这些失败看起来和"UI 坏了"一模一样。
add_executable(fox_headless ${FOX_DIR}/fox_headless.cpp)
target_link_libraries(fox_headless PRIVATE fox_core)
if(WIN32)
    target_link_libraries(fox_headless PRIVATE ws2_32 gdi32 user32)
    set_target_properties(fox_headless PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# TextBuffer 单元测试。只链接 text_buffer.cpp——无窗口、无 GPU、无渲染器、
# 无字体管理器。刻意做成最小目标：编辑模型的难点（光标不落在多字节码点内部、
# 连续输入合并成一步撤销）与渲染无关，经由渲染器测试就意味着 GPU 坏了会让纯
# 逻辑测试失败。
#
# 这个目标抓到过一个 bug，证明它值得存在：向后退一格的 UTF-8 步进返回了光标
# 自身位置——退格在 ASCII 上静默无效，按词左移让程序死循环。这两个 bug 都没法
# 靠跑程序看界面发现。
add_executable(fox_text_test ${FOX_DIR}/fox_text_test.cpp ${FOX_DIR}/text/text_buffer.cpp)
# 不链接 fox_core：该目标必须与渲染器、字体管理器、窗口层绝缘，否则 GPU 坏了
# 会让纯逻辑测试失败。因此它自带 include 路径。
target_include_directories(fox_text_test PRIVATE ${FOX_DIR})
if(WIN32)
    set_target_properties(fox_text_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

add_executable(fox_metrics_test tests/fox_metrics_test.cpp ${FOX_DIR}/text/font.cpp)
# 不链接 fox_core：字体加载与字形度量只需要字体栈。会因为缺 GPU 而失败的目标
# 不出一周就会被关掉，而这个测试必须在完全没有 Vulkan 的机器上持续运行。
target_include_directories(fox_metrics_test PRIVATE ${FOX_DIR})
if(WIN32)
    set_target_properties(fox_metrics_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# 软换行算法（text/soft_wrap.hpp）。
#
# 与 fox_metrics_test 一样只依赖字体栈：wrap_line 是对字形宽度做纯布局运算，
# 它必须保持的不变量（行连续、行覆盖整行、不丢内容、在空格后断行、码点不拆分）
# 恰恰是截图看不出来的。断行点丢一个字节在屏幕上不可见，却会破坏使用它的控件
# 里所有点击/按键/选区偏移。
add_executable(fox_wrap_test tests/fox_wrap_test.cpp ${FOX_DIR}/text/font.cpp)
target_include_directories(fox_wrap_test PRIVATE ${FOX_DIR})
if(WIN32)
    set_target_properties(fox_wrap_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# 组合控件逻辑（菜单/组合框/可选中项/分组/属性）。
#
# 与 fox_host_test 一样链接 fox_core：控件是行为，行为需要真实的 Context（状态、
# 焦点、输入、显示列表），mock 不行。不创建窗口和 GPU；测试直接驱动
# frame_begin / 合成输入 / frame_end，与无头光栅化器用的是同一套测试框架。它还
# 钉住了 InputState::k_index 修复（具名键曾被用于索引 keys_pressed[-1]）。
add_executable(fox_widgets_test tests/fox_widgets_test.cpp)
target_link_libraries(fox_widgets_test PRIVATE fox_core)
if(WIN32)
    target_link_libraries(fox_widgets_test PRIVATE ws2_32 gdi32 user32)
    set_target_properties(fox_widgets_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# 像素加速器一致性测试（utils/Pa.hpp）。
#
# 正是这个测试让加速器可信而不只是"看起来没问题"：同一帧渲染两次，一次开加速、
# 一次关加速（逐像素参考路径），两个缓冲逐字节比较。加速器改动的任何东西都会在
# 这里暴露为偏移。
#
# 它已经三次证明了自己的价值，抓到过：(1) 核心区推导差了一个圆角半径；
# (2) "超出容量 → 重绘整个裁剪区"的回退路径重绘了干净 tile——在已合成的像素上
# 重新混合半透明边缘不是幂等的，会把某个通道悄悄偏移 1；(3) damage 矩形钳制了 x
# 却没钳制 y0，图像向上溢出到自身顶边以上 15 行（有覆盖率测试的图元看不出来，
# 纹理则立刻暴露）。
add_executable(pa_parity_test tests/pa_parity_test.cpp)
target_link_libraries(pa_parity_test PRIVATE fox_core)
if(WIN32)
    target_link_libraries(pa_parity_test PRIVATE ws2_32 gdi32 user32)
    set_target_properties(pa_parity_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# PNG / inflate / mipmap 链测试（image/png.cpp）。
#
# 无需 GPU 或窗口即可验证，证据是字节精确而非视觉的：zlib 自己的流必须解码回
# 原始字节；RFC1951 三种块类型必须真正被跑到（计数验证，不是假定）；32768 LZ77
# 窗口边界是手写的 fixed-Huffman 流；每个非法流/PNG 必须因正确的原因失败
# （断言检查错误消息，所以一律拒绝的解码器过不了）。
#
# 像素判据（pa_parity_test 场景 [7]）负责证明连线正确：光栅化缩小对比最近邻
# 回退，开关加速逐字节一致。
add_executable(fox_png_test tests/fox_png_test.cpp)
target_link_libraries(fox_png_test PRIVATE fox_core)
if(WIN32)
    target_link_libraries(fox_png_test PRIVATE ws2_32 gdi32 user32)
    set_target_properties(fox_png_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# .rvdlm → safetensors / gguf 容器转换。
#
# 纯头文件（qml/ModelInterop.hpp），在无法配置面向 Vulkan 的运行时的机器上也能
# 构建。测试自带极简读取器，写出的产物由 runtime/tests/model_interop_crosscheck.py
# 用 Python 重新解析：三份独立实现，因为"我们能读回自己写的"只证明自洽，
# 不证明合规。
# nqcmath（普通数学 + 并行层）与 expermath（量子数学）的已知答案测试。
# 纯头文件、只依赖 STL 与 OpenMP —— 不链 rvdl_rt，这样它可以在任何
# 机器上跑（包括没有 GPU、没有 Vulkan ICD 的那台）。
add_executable(numqk_math_test tests/numqk_math_test.cpp)
target_include_directories(numqk_math_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(numqk_math_test PRIVATE OpenMP::OpenMP_CXX)
endif()
if(MSVC)
    target_compile_options(numqk_math_test PRIVATE /O2)
else()
    target_compile_options(numqk_math_test PRIVATE -O2)
endif()

# pB（点参数）测试：超分 + 超量化 + 拓扑流形压缩成点。纯头文件 + STL，
# 无 QVM/Kokkos —— 在无 GPU 的机器上也能跑。
add_executable(pb_test tests/pb_test.cpp)
target_include_directories(pb_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(pb_test PRIVATE /O2)
else()
    target_compile_options(pb_test PRIVATE -O2)
endif()

# 量子时间缩微调度（tau 缩放，韬定律公式改造）测试：代际缩放 / 折叠深度 /
# 齿轮比 / 关键路径 DP / 时间影子价格 / 预算路由的性质断言。
# 只依赖头文件 + STL，不链 rvdl_rt。
add_executable(tau_scale_test tests/tau_scale_test.cpp)
target_include_directories(tau_scale_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(tau_scale_test PRIVATE /O2)
else()
    target_compile_options(tau_scale_test PRIVATE -O2)
endif()

# LF2C（无损特征快速上下文压缩）测试：块/池/协调者三层的性质断言。
# 只依赖头文件 + STL（mock 后端在本测试文件里），不链 rvdl_rt —— 这样
# 它在没有 GPU 的机器上也能跑。
add_executable(lf2c_test tests/lf2c_test.cpp)
target_include_directories(lf2c_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(lf2c_test PRIVATE OpenMP::OpenMP_CXX)
endif()
if(MSVC)
    target_compile_options(lf2c_test PRIVATE /O2)
else()
    target_compile_options(lf2c_test PRIVATE -O2)
endif()

add_executable(model_interop_test tests/model_interop_test.cpp)
target_include_directories(model_interop_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(model_interop_test PRIVATE /O2)
else()
    target_compile_options(model_interop_test PRIVATE -O2)
endif()

# Android 专用优化层（utils/A2a.hpp）。
#
# 不需要真机：每个探测都接受可注入的 sysfs 根目录，测试因此能覆盖各种真实形态
# （多簇、容量抖动、不可读 zone、整数摄氏度驱动、不能驱动温控档位的电池 zone）。
# "只能在手机上测"等于"从没测过"。
add_executable(a2a_test tests/a2a_test.cpp)
target_include_directories(a2a_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(a2a_test PRIVATE /O2)
else()
    target_compile_options(a2a_test PRIVATE -O2)
endif()
if(WIN32)
    set_target_properties(a2a_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# cgui_*/cgfx_* 的运行时后端选择（include/qhal/FoxHost.hpp）。
#
# 不链接 fox_core、无 GPU、无窗口、无 Vulkan：宿主通过 C ABI 函数表与 fox 对接，
# 窗口是函数指针接缝，"fox 优先、失败退回 Nuklear、失败后不重试"是逐帧对着一个
# 记录每次调用的假 fox 驱动验证的。这也让三个初始化失败关卡（fox_create 返回空 /
# init_backend 返回 0 / backend_ready 返回 0）不至于悄悄失效：每个都必须落到
# Nuklear 并说明原因。
add_executable(fox_host_test tests/fox_host_test.cpp)
target_include_directories(fox_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                ${FOX_DIR})
if(MSVC)
    target_compile_options(fox_host_test PRIVATE /O2)
else()
    target_compile_options(fox_host_test PRIVATE -O2)
endif()
if(WIN32)
    set_target_properties(fox_host_test PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# ==========================================
# 7. TARGET: RVDLRSP
# ==========================================
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../RSP rvdlRSP)

# ==========================================
# 8. TARGETS: 基准与示例
#
#    物理/数学验证基准（可选，默认 OFF）：
#        cmake -DRVDL_BUILD_BENCHMARKS=ON ..
#
#    GUI/框架示例（可选，默认 OFF）：
#        cmake -DRVDL_BUILD_EXAMPLES=ON ..
#
#    自测（qcos_smoke / qchain_selftest / mir_module_test）始终构建——
#    它们验证正确性，不是演示功能。
# ==========================================

# --- 物理/数学基准 ----------------------------------------------------------
option(RVDL_BUILD_BENCHMARKS "Build physics/math validation benchmarks" OFF)

if(RVDL_BUILD_BENCHMARKS)
    # 核心时空验证：4 个数值基准
    add_executable(spacetime_benchmark benchmarks/spacetime_benchmark.cpp)
    target_include_directories(spacetime_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(spacetime_benchmark PRIVATE /O2)
    else()
        target_compile_options(spacetime_benchmark PRIVATE -O2)
    endif()

    # 扩展模块：谱方法、S²、高亏格、KK、O(D,D)
    add_executable(spacetime_extended_benchmark benchmarks/spacetime_extended_benchmark.cpp)
    target_include_directories(spacetime_extended_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(spacetime_extended_benchmark PRIVATE /O2)
    else()
        target_compile_options(spacetime_extended_benchmark PRIVATE -O2)
    endif()

    # 神经场 + EEG 谱分析
    add_executable(neural_eeg_benchmark benchmarks/neural_eeg_benchmark.cpp)
    target_include_directories(neural_eeg_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(neural_eeg_benchmark PRIVATE /O2)
    else()
        target_compile_options(neural_eeg_benchmark PRIVATE -O2)
    endif()

    # 畴壁成核可视化（输出 PGM 快照）
    add_executable(nucleation_visualization benchmarks/nucleation_visualization.cpp)
    target_include_directories(nucleation_visualization PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(nucleation_visualization PRIVATE /O2)
    else()
        target_compile_options(nucleation_visualization PRIVATE -O2)
    endif()

    # 穿梭/追踪规划器：四策略代价表、auto 启发式、无截断内核（logWeight 永不下溢）、
    # 保真阶梯。纯头文件（utils/Ta.hpp），无链接依赖，在无法配置面向 Vulkan 的
    # 运行时的机器上也能构建。
    add_executable(shuttle_plan_benchmark benchmarks/shuttle_plan_benchmark.cpp)
    target_include_directories(shuttle_plan_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(shuttle_plan_benchmark PRIVATE /O2)
    else()
        target_compile_options(shuttle_plan_benchmark PRIVATE -O2)
    endif()

    # 像素加速器（utils/Pa.hpp）跑在 Fox CPU 后端上：damage 跟踪 / 核心区填充 /
    # 遮挡剔除到底省了多少，没省的时候时间花在哪（哈希本身有开销，全屏动画那一行
    # 就是它显形的地方）。
    add_executable(pa_pixel_benchmark benchmarks/pa_pixel_benchmark.cpp)
    target_link_libraries(pa_pixel_benchmark PRIVATE fox_core)
    if(MSVC)
        target_compile_options(pa_pixel_benchmark PRIVATE /O2)
    else()
        target_compile_options(pa_pixel_benchmark PRIVATE -O2)
    endif()

    # 便捷元目标：运行全部基准
    add_custom_target(run_benchmarks
        COMMAND spacetime_benchmark
        COMMAND spacetime_extended_benchmark
        COMMAND neural_eeg_benchmark
        COMMAND nucleation_visualization
        COMMAND shuttle_plan_benchmark
        COMMAND pa_pixel_benchmark
        DEPENDS spacetime_benchmark spacetime_extended_benchmark neural_eeg_benchmark nucleation_visualization shuttle_plan_benchmark pa_pixel_benchmark
        COMMENT "Running all physics/math validation benchmarks"
        VERBATIM
    )
endif()

# --- GUI/框架示例 -----------------------------------------------------------
option(RVDL_BUILD_EXAMPLES "Build GUI/framework example applications" OFF)

if(RVDL_BUILD_EXAMPLES)
    # Fox GUI 框架展示（macOS 27 Golden Gate 风格）
    add_executable(fox_showcase examples/gui/fox_showcase.cpp)
    target_include_directories(fox_showcase PRIVATE ${FOX_DIR})
    target_link_libraries(fox_showcase PRIVATE fox_core)
    if(WIN32)
        target_link_libraries(fox_showcase PRIVATE ws2_32 gdi32 user32)
        set_target_properties(fox_showcase PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
endif()

# ==========================================
# 9. Linux rpath：让可执行文件从构建目录 / 自身目录加载 librvdl_rt.so
# ==========================================
if(UNIX AND NOT APPLE)
    set_target_properties(runtime transmitter qvm_visualizer PROPERTIES
        BUILD_RPATH "$<TARGET_FILE_DIR:rvdl_rt>"
        INSTALL_RPATH "\$ORIGIN")
endif()

# 打包:跳过 install 阶段的 RPATH 改写,避免 CPack 打包阶段
# RPATH_CHANGE 因 RPATH 冒号格式不匹配而失败(nvcc_wrapper 环境特有)。
# 代价:安装后的程序依赖系统 LD_LIBRARY_PATH 定位 librvdl_rt.so。
set(CMAKE_SKIP_INSTALL_RPATH ON CACHE BOOL "Skip RPATH rewrite during install" FORCE)
set(CMAKE_SKIP_RPATH ON CACHE BOOL "Skip all RPATH generation" FORCE)

# ==========================================
# 10. 安装规则 + CPack 打包
# ==========================================
install(TARGETS rvdl_rt rvdl_rt_host
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(TARGETS runtime transmitter qvm_visualizer RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

# 打包元数据
set(CPACK_PACKAGE_NAME "ravendrel")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Ravendrel quantum robot simulation platform (rvdlRSP)")
set(CPACK_PACKAGE_VENDOR "Ravendrel")
set(CPACK_PACKAGE_CONTACT "ravendrel@example.com")
set(CPACK_GENERATOR "TGZ")
if(UNIX AND NOT APPLE)
    list(APPEND CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ravendrel <ravendrel@example.com>")
endif()
include(CPack)