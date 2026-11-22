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
  RVDL_RT_API const char *rvdl_runtime_bind_mmi(rvdl_runtime *rt, const char *alias, const char *path);
  RVDL_RT_API const char *rvdl_runtime_mmi_invoke(rvdl_mmi *m,
                                                    const char *func_name,
                                                    const char *args_json);
  RVDL_RT_API void rvdl_runtime_mmi_unload(rvdl_mmi *m);

  RVDL_RT_API const char *rvdl_runtime_qchain_wallet(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_mint(rvdl_runtime *rt, const char *addr, uint64_t amount);
  RVDL_RT_API const char *rvdl_runtime_qchain_transfer(rvdl_runtime *rt, const char *from, const char *to, uint64_t amount);
  RVDL_RT_API const char *rvdl_runtime_qchain_balance(rvdl_runtime *rt, const char *addr);
  RVDL_RT_API const char *rvdl_runtime_qchain_mine(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_height(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_verify(rvdl_runtime *rt);
  RVDL_RT_API const char *rvdl_runtime_qchain_qkd(rvdl_runtime *rt, int32_t rounds);
  RVDL_RT_API const char *rvdl_runtime_qchain_qdba(rvdl_runtime *rt, int32_t parties);

  RVDL_RT_API int32_t rvdl_runtime_load_native(rvdl_runtime *rt, const char *path);
  RVDL_RT_API void rvdl_runtime_register_native_symbol(rvdl_runtime *rt, const char *name, void *addr);

  RVDL_RT_API int32_t rvdl_runtime_run_topology(const char *json);

  RVDL_RT_API const char *rvdl_runtime_compile_mir(rvdl_runtime *rt, const char *mir_json);

#ifdef __cplusplus
}
#endif