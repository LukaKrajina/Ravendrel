// ============================================================================
// MirModuleBuilder 下沉测试（MIR JSON → LLVM Module → verifyModule）
//
// 覆盖 server 端 mir-serialize.ts 序列化出的 MIR 在 C++ 端重建为合法 LLVM
// IR 的全链路。每个用例断言 build() 成功（即 verifyModule 通过）或按预期
// 拒绝（非法 gate / 非法语句 / 结构缺失）。
//
// 仅依赖 LLVM（Core/Support），不依赖 Kokkos/Vulkan/GLFW。
// ============================================================================
#include <iostream>
#include <string>

#include "qhal/MirModuleBuilder.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            ++g_failures;                                                      \
            std::cerr << "FAIL: " << #cond << " (line " << __LINE__ << ")\n";  \
        }                                                                      \
    } while (0)

// 期望 build 成功（verifyModule 通过）
static void expect_ok(const char *name, const char *mir)
{
    qhal::MirModuleBuilder builder; // 每个用例独立实例，避免状态残留
    try
    {
        builder.build(mir);
        std::cout << "[OK] " << name << "\n";
    }
    catch (const std::exception &e)
    {
        ++g_failures;
        std::cerr << "[FAIL] " << name << ": " << e.what() << "\n";
    }
}

// 期望 build 被拒绝（抛异常）
static void expect_reject(const char *name, const char *mir)
{
    qhal::MirModuleBuilder builder;
    try
    {
        builder.build(mir);
        ++g_failures;
        std::cerr << "[FAIL] " << name << ": expected rejection but succeeded\n";
    }
    catch (const std::exception &e)
    {
        std::cout << "[OK] " << name << " (rejected: " << e.what() << ")\n";
    }
}

// ---- 基础：常量 / 变量 / 二元 / 比较 ----------------------------------------
static void test_basic()
{
    // 常量返回
    expect_ok("const_return", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[],"blocks":[{"id":0,"statements":[],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":42,"ty":"int32"}}}}]}]})");

    // 变量赋值 + 二元运算 + 返回值
    expect_ok("binary_add", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"a","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"b","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":2,"name":"c","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Use","operand":{"kind":"Const","value":{"kind":"Int","value":10,"ty":"int32"}}}},{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"Use","operand":{"kind":"Const","value":{"kind":"Int","value":32,"ty":"int32"}}}},{"kind":"Assign","place":{"local":2,"projection":[]},"rvalue":{"kind":"Binary","op":"+","lhs":{"kind":"Copy","place":{"local":0,"projection":[]}},"rhs":{"kind":"Copy","place":{"local":1,"projection":[]}}}}],"terminator":{"kind":"Return","value":{"kind":"Copy","place":{"local":2,"projection":[]}}}}]}]})");

    // 比较运算（icmp）
    expect_ok("compare_lt", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Binary","op":"<","lhs":{"kind":"Const","value":{"kind":"Int","value":1,"ty":"int32"}},"rhs":{"kind":"Const","value":{"kind":"Int","value":2,"ty":"int32"}}}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    // 浮点除法（fdiv）
    expect_ok("fdiv", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"double","params":[],"locals":[{"id":0,"name":"r","ty":"double","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Binary","op":"/","lhs":{"kind":"Const","value":{"kind":"Float","value":1.5,"ty":"double"}},"rhs":{"kind":"Const","value":{"kind":"Float","value":2.0,"ty":"double"}}}}],"terminator":{"kind":"Return","value":{"kind":"Copy","place":{"local":0,"projection":[]}}}}]}]})");

    // 字符串常量（全局字符串）
    expect_ok("str_const", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"string","params":[],"locals":[],"blocks":[{"id":0,"statements":[],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Str","value":"hello","ty":"string"}}}}]}]})");
}

// ---- 量子：分配 / 门 / 测量 ------------------------------------------------
static void test_quantum()
{
    // 单 qubit：h + measure
    expect_ok("quantum_h", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"q","ty":"Qubit","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1},{"id":2,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"QAlloc","ty":"Qubit"}},{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"QGate","gate":"h","args":[{"kind":"Copy","place":{"local":0,"projection":[]}}]}},{"kind":"Assign","place":{"local":2,"projection":[]},"rvalue":{"kind":"QMeasure","arg":{"kind":"Copy","place":{"local":0,"projection":[]}},"ty":"int32"}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    // 双 qubit：cnot
    expect_ok("quantum_cnot", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"q1","ty":"Qubit","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"q2","ty":"Qubit","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":2,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"QAlloc","ty":"Qubit"}},{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"QAlloc","ty":"Qubit"}},{"kind":"Assign","place":{"local":2,"projection":[]},"rvalue":{"kind":"QGate","gate":"cnot","args":[{"kind":"Copy","place":{"local":0,"projection":[]}},{"kind":"Copy","place":{"local":1,"projection":[]}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    // rz（double 角度）+ qft（i32）
    expect_ok("quantum_rz_qft", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"q","ty":"Qubit","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"QAlloc","ty":"Qubit"}},{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"QGate","gate":"rz","args":[{"kind":"Const","value":{"kind":"Float","value":1.5708,"ty":"double"}},{"kind":"Copy","place":{"local":0,"projection":[]}}]}},{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"QGate","gate":"qft","args":[{"kind":"Const","value":{"kind":"Int","value":3,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    // 用户自定义门（@[gate] 函数）：my_gate 定义 + rvdl_main 内 QGate(gate="my_gate") → call @my_gate
    expect_ok("quantum_user_gate", R"({"version":1,"bodies":[{"owner":"my_gate","returnTy":"void","params":[0],"locals":[{"id":0,"name":"q","ty":"Qubit","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"QGate","gate":"h","args":[{"kind":"Copy","place":{"local":0,"projection":[]}}]}}],"terminator":{"kind":"Return","value":null}}]},{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"q","ty":"Qubit","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"QAlloc","ty":"Qubit"}},{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"QGate","gate":"my_gate","args":[{"kind":"Copy","place":{"local":0,"projection":[]}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");
}

// ---- 原子：sync_load / store / add / cas -----------------------------------
static void test_atomic()
{
    expect_ok("sync_add", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"c","ty":"cap<int32>","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"Call","target":"sync_add","retTy":"int32","args":[{"kind":"Copy","place":{"local":0,"projection":[]}},{"kind":"Const","value":{"kind":"Int","value":1,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_ok("sync_store_load", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"c","ty":"cap<int32>","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"v","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"Call","target":"sync_store","retTy":"void","args":[{"kind":"Copy","place":{"local":0,"projection":[]}},{"kind":"Const","value":{"kind":"Int","value":7,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_ok("sync_cas", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"c","ty":"cap<int32>","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"Call","target":"sync_cas","retTy":"int32","args":[{"kind":"Copy","place":{"local":0,"projection":[]}},{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":1,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");
}

// ---- 特殊：端口 I/O / 地址 ------------------------------------------------
static void test_special()
{
    expect_ok("outb", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Call","target":"outb","retTy":"void","args":[{"kind":"Const","value":{"kind":"Int","value":80,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":65,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_ok("inb", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Call","target":"inb","retTy":"int32","args":[{"kind":"Const","value":{"kind":"Int","value":96,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Copy","place":{"local":0,"projection":[]}}}}]}]})");

    expect_ok("addr", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"x","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"Call","target":"addr","retTy":"int32","args":[{"kind":"Ref","place":{"local":0,"projection":[]}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");
}

// ---- 内建：量子对象 / GUI / 图形 ------------------------------------------
static void test_builtins()
{
    expect_ok("bell_state", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"s","ty":"QObject","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Call","target":"rvdl_create_BellState","retTy":"QObject","args":[]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_ok("gui_init", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"t","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1},{"id":1,"name":"r","ty":"int32","mut":true,"linear":false,"temporary":false,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":1,"projection":[]},"rvalue":{"kind":"Call","target":"cgui_init","retTy":"int32","args":[{"kind":"Const","value":{"kind":"Int","value":640,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":480,"ty":"int32"}},{"kind":"Ref","place":{"local":0,"projection":[]}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_ok("cgfx_rect", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"Call","target":"cgfx_rect","retTy":"void","args":[{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":100,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":100,"ty":"int32"}},{"kind":"Const","value":{"kind":"Int","value":255,"ty":"int32"}}]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");
}

// ---- 拒绝：非法 gate / 非法语句 / 结构缺失 --------------------------------
static void test_rejects()
{
    expect_reject("unknown_gate", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[{"id":0,"name":"","ty":"void","mut":false,"linear":false,"temporary":true,"line":1,"column":1}],"blocks":[{"id":0,"statements":[{"kind":"Assign","place":{"local":0,"projection":[]},"rvalue":{"kind":"QGate","gate":"nonexistent_gate","args":[]}}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_reject("unknown_statement", R"({"version":1,"bodies":[{"owner":"rvdl_main","returnTy":"int32","params":[],"locals":[],"blocks":[{"id":0,"statements":[{"kind":"NoSuchStatement"}],"terminator":{"kind":"Return","value":{"kind":"Const","value":{"kind":"Int","value":0,"ty":"int32"}}}}]}]})");

    expect_reject("missing_bodies", R"({"version":1})");
}

int main()
{
    test_basic();
    test_quantum();
    test_atomic();
    test_special();
    test_builtins();
    test_rejects();

    if (g_failures == 0)
    {
        std::cout << "ALL MIR MODULE TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " MIR MODULE TEST(S) FAILED\n";
    return 1;
}
