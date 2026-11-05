#pragma once
#include <cstdint>
#if defined(_WIN32)
#if defined(RVDL_RT_BUILD)
#define RVDL_RT_API __declspec(dllexport)
#else
#define RVDL_RT_API __declspec(dllimport)
#endif
#else
#define RVDL_RT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct rvdl_runtime rvdl_runtime;

  RVDL_RT_API rvdl_runtime *rvdl_runtime_create(void);
  RVDL_RT_API void rvdl_runtime_destroy(rvdl_runtime *rt);
  RVDL_RT_API void rvdl_runtime_viz_start(rvdl_runtime *rt);
  RVDL_RT_API void rvdl_runtime_viz_stop(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_compile(rvdl_runtime *rt, const char *ir);
  RVDL_RT_API const char *rvdl_runtime_execute_int(rvdl_runtime *rt, const char *func_name);
  RVDL_RT_API const char *rvdl_runtime_execute_float(rvdl_runtime *rt, const char *func_name);
  RVDL_RT_API const char *rvdl_runtime_execute_void(rvdl_runtime *rt, const char *func_name);
  RVDL_RT_API const char *rvdl_runtime_aot_compile(rvdl_runtime *rt,
                                                     const char *arch,
                                                     const char *mode,
                                                     const char *output_name,
                                                     const char *ir);
  // 扩展版：额外传入 Android 交叉编译工具链覆盖项（可为 nullptr，回退进程环境变量）。
  RVDL_RT_API const char *rvdl_runtime_aot_compile_ex(rvdl_runtime *rt,
                                                        const char *arch,
                                                        const char *mode,
                                                        const char *output_name,
                                                        const char *ir,
                                                        const char *clangxx,
                                                        const char *sysroot,
                                                        const char *rt_dir,
                                                        const char *extra_libs);
  RVDL_RT_API const char *rvdl_runtime_snapshot(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_verify(rvdl_runtime *rt, const char *vc_protocol);

  typedef struct rvdl_mmi rvdl_mmi;

  RVDL_RT_API const char *rvdl_runtime_export_mmi(rvdl_runtime *rt,
                                                    const char *header_json,
                                                    const char *ir,
                                                    const char *output_path);
  RVDL_RT_API rvdl_mmi *rvdl_runtime_load_mmi(rvdl_runtime *rt, const char *path);
  // 加载 .rvdli 并绑定其导出到主 JIT（供主程序 import .rvdli 时解析 declare 符号）
  RVDL_RT_API const char *rvdl_runtime_bind_mmi(rvdl_runtime *rt, const char *alias, const char *path);
  RVDL_RT_API const char *rvdl_runtime_mmi_invoke(rvdl_mmi *m,
                                                    const char *func_name,
                                                    const char *args_json);
  RVDL_RT_API void rvdl_runtime_mmi_unload(rvdl_mmi *m);

  // ─── QChain 量子区块链服务 ABI ─────────────────────────────────
  // 供外部进程（daemon socket 协议 / CLI / HTTP / AOT 产物）直接调用量子币种与
  // 量子区块链服务，无需经过 rvdl 源码。内部复用 global_qm 后端的 QChainService 单例。
  RVDL_RT_API const char *rvdl_runtime_qchain_wallet(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_mint(rvdl_runtime *rt, const char *addr, uint64_t amount);
  RVDL_RT_API const char *rvdl_runtime_qchain_transfer(rvdl_runtime *rt, const char *from, const char *to, uint64_t amount);
  RVDL_RT_API const char *rvdl_runtime_qchain_balance(rvdl_runtime *rt, const char *addr);
  RVDL_RT_API const char *rvdl_runtime_qchain_mine(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_height(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_verify(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_qkd(rvdl_runtime *rt, int32_t rounds);
  RVDL_RT_API const char *rvdl_runtime_qchain_qdba(rvdl_runtime *rt, int32_t parties);

  // ─── 原生扩展库加载（供 .rvdli 调用外部 C/C++ 库符号）──────────────────
  // dlopen/LoadLibrary 一个动态库，使其符号进入进程，供 .rvdli 的 JIT 解析
  // （SandboxJIT 的进程符号生成器允许非 qk_ 前缀符号，如 steam_*）。
  RVDL_RT_API int32_t rvdl_runtime_load_native(rvdl_runtime *rt, const char *path);
  // 登记原生符号（供 SandboxJIT 绑定动态库符号）
  RVDL_RT_API void rvdl_runtime_register_native_symbol(rvdl_runtime *rt, const char *name, void *addr);

  // ─── 多维标签函数执行拓扑（@layer）调度入口 ───────────────────────────
  // 由 IR 生成的 rvdl_topology_entry 调用：解析调度表 JSON（shape + blocks），
  // 通过活跃 runtime 的 JIT 查找各块符号，按 (time, thread, coord) 调度执行。
  // json = { shape: {time, thread, coord[]}, blocks: [{name, time, thread, coord[], cost, deadline}] }
  RVDL_RT_API int32_t rvdl_runtime_run_topology(const char *json);

  // ─── MIR 编译（下沉 LLVM C++ API）────────────────────────────────────────
  // 解析 MIR JSON（server 端 mir-serialize.ts 序列化），用 MirModuleBuilder
  // 经 LLVM C++ API 构建 Module 并加载进 JIT。payload = MIR JSON。
  RVDL_RT_API const char *rvdl_runtime_compile_mir(rvdl_runtime *rt, const char *mir_json);

#ifdef __cplusplus
}
#endif