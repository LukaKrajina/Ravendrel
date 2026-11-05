// ============================================================================
// MirModuleBuilder
// 把 server 端序列化的 MIR（CFG）用 LLVM C++ API 构建为LLVM Module。
//
// 与字符串拼接（旧 ir.ts）不同：这里用 llvm::IRBuilder 构建SSA IR，
// 构建后经 llvm::verifyModule 校验，并可挂优化 pass（mem2reg / instcombine）。
//
// MIR 序列化格式见 server/src/mir-serialize.ts：
//   { version: 1, bodies: [ { owner, returnTy, params, locals, blocks } ] }
//
// 核心映射（MIR → LLVM）：
//   MirLocalDecl               → entry 块内 alloca（变量以 load/store 访问，mem2reg 可优化）
//   Assign(place = rvalue)     → 求值 rvalue 后 store 到 place 的 alloca
//   Use(Copy/Move/Ref place)   → load place 的 alloca
//   Const(Int/Float/Str/...)   → ConstantInt / ConstantFP / 全局字符串
//   Binary(op)                 → add/sub/mul/sdiv/fdiv/icmp 等
//   Call(target, args, retTy)  → declare + call
//   Goto(target)               → br label %target
//   SwitchInt(disc, targets)   → switch / br（条件二分支时）
//   Return(value)              → ret
//
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <stdexcept>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/raw_ostream.h"

#include "json.hpp" // qhal::json（独立解析器，避免 MMI → JIT → vendor 依赖链）

namespace qhal
{

/** MIR 构建产物：Module 与其 LLVMContext 必须成对传给 JIT（ThreadSafeModule 需要）。
 *  注意：context 必须声明在 module 之前，使 module 先析构、context 后析构
 *  （Module 持有 LLVMContext& 引用，若 context 先析构会悬垂崩溃）。 */
struct BuiltMirModule
{
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
};

class MirModuleBuilder
{
public:
    BuiltMirModule build(const std::string &mir_json, const std::string &module_name = "mir_module")
    {
        ctx_ = std::make_unique<llvm::LLVMContext>();
        module_ = std::make_unique<llvm::Module>(module_name, *ctx_);
        builder_ = std::make_unique<llvm::IRBuilder<>>(*ctx_);

        json::Value doc = json::parse(mir_json);
        if (!doc.is_object() || !doc.has("bodies") || !doc.at("bodies").is_array())
            throw std::runtime_error("MIR: missing bodies array");

        // 解析 form 定义，建立结构类型 { i8* vtable, fields... }（字段索引从 1 开始）。
        // @[packed] form：无 vtable、无填充，字段从偏移 0 起（isPacked=true）。
        form_types_.clear();
        if (doc.has("forms") && doc.at("forms").is_array())
        {
            for (const auto &fv : doc.at("forms").array())
            {
                const std::string fname = fv.at("name").string();
                const bool packed = fv.has("packed") && fv.at("packed").bool_value();
                std::vector<llvm::Type *> elems;
                if (!packed)
                    elems.push_back(llvm::PointerType::get(*ctx_, 0)); // vtable 指针
                for (const auto &f : fv.at("fields").array())
                    elems.push_back(mapType(f.at("ty").string()));
                form_types_[fname] = llvm::StructType::create(*ctx_, elems, "form." + fname, packed);
            }
        }

        // 第一遍：为每个 body 预声明函数（支持前向调用）。
        // 参数类型需从 locals 表取：先收集每个 body 的「param local id → 类型」映射。
        std::unordered_map<std::string, std::vector<llvm::Type *>> param_types_by_owner;
        for (const auto &bv : doc.at("bodies").array())
        {
            const std::string owner = bv.at("owner").string();
            // locals 表：id → ty
            std::unordered_map<int, std::string> local_ty;
            if (bv.has("locals"))
            {
                for (const auto &l : bv.at("locals").array())
                    local_ty[l.at("id").int_value()] = l.at("ty").string();
            }
            std::vector<llvm::Type *> param_types;
            if (bv.has("params"))
            {
                for (const auto &pid : bv.at("params").array())
                {
                    int id = pid.int_value();
                    auto it = local_ty.find(id);
                    param_types.push_back(it != local_ty.end() ? mapType(it->second) : llvm::Type::getInt32Ty(*ctx_));
                }
            }
            param_types_by_owner[owner] = param_types;
        }

        for (const auto &bv : doc.at("bodies").array())
        {
            const std::string owner = bv.at("owner").string();
            const std::string ret_ty = bv.has("returnTy") && !bv.at("returnTy").is_null()
                                           ? bv.at("returnTy").string()
                                           : std::string("void");
            llvm::FunctionType *ft = llvm::FunctionType::get(mapType(ret_ty), param_types_by_owner[owner], false);
            llvm::Function *fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, owner, module_.get());
            funcs_[owner] = fn;
        }

        // 第二遍：逐个 body 填充函数体
        for (const auto &bv : doc.at("bodies").array())
        {
            buildBody(bv);
        }

        // 拓扑入口：从 @layer 标签聚合调度表，生成 rvdl_topology_entry
        emitTopologyEntry(doc);

        // 校验
        std::string err;
        llvm::raw_string_ostream os(err);
        if (llvm::verifyModule(*module_, &os))
            throw std::runtime_error("MIR: verifyModule failed:\n" + err);

        BuiltMirModule out;
        out.module = std::move(module_);
        builder_.reset();              // builder 持有 ctx_ 引用，先释放避免悬垂
        out.context = std::move(ctx_); // 再安全 move ctx_
        return out;
    }

private:
    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::unordered_map<std::string, llvm::Function *> funcs_;
    std::unordered_map<int, llvm::AllocaInst *> local_alloca_;  // local id -> alloca
    std::unordered_map<int, std::string> local_types_;          // local id -> rvdl 类型名（Drop/Consume 释放用）
    std::unordered_map<std::string, llvm::StructType *> form_types_; // form 名 -> 结构类型
    llvm::Function *current_fn_ = nullptr;
    std::unordered_map<int, llvm::BasicBlock *> blocks_;   // block id -> BasicBlock

    // ---- 类型映射 -----------------------------------------------------------
    static std::string trimCopy(std::string s)
    {
        const char *ws = " \t\r\n";
        s.erase(0, s.find_first_not_of(ws));
        s.erase(s.find_last_not_of(ws) + 1);
        return s;
    }

    llvm::Type *mapType(const std::string &ty)
    {
        auto &c = *ctx_;
        if (ty == "int8" || ty == "uint8" || ty == "char" || ty == "bool") return llvm::Type::getInt8Ty(c);
        if (ty == "int16" || ty == "uint16") return llvm::Type::getInt16Ty(c);
        if (ty == "int" || ty == "int32" || ty == "uint32") return llvm::Type::getInt32Ty(c);
        if (ty == "int64" || ty == "uint64") return llvm::Type::getInt64Ty(c);
        if (ty == "float") return llvm::Type::getFloatTy(c);
        if (ty == "double") return llvm::Type::getDoubleTy(c);
        if (ty == "complex64")
            return llvm::StructType::get(c, {llvm::Type::getFloatTy(c), llvm::Type::getFloatTy(c)});
        if (ty == "complex128")
            return llvm::StructType::get(c, {llvm::Type::getDoubleTy(c), llvm::Type::getDoubleTy(c)});
        if (ty == "void") return llvm::Type::getVoidTy(c);
        if (ty == "string" || ty == "i8*") return llvm::PointerType::get(c, 0);
        if (ty == "Qubit" || ty == "QObject" || ty == "QModel" || ty == "QReservoir" || ty == "Lattice")
            return llvm::PointerType::get(c, 0); // opaque 指针
        if (ty.rfind("cap<", 0) == 0)
            return llvm::PointerType::get(c, 0); // 能力指针 → 裸指针
        if (ty.rfind("lattice<", 0) == 0)
            return llvm::PointerType::get(c, 0);
        // 定长数组 arr<T, N> → [N x T]
        if (ty.rfind("arr<", 0) == 0 && ty.size() > 4 && ty.back() == '>')
        {
            std::string inner = ty.substr(4, ty.size() - 5);
            size_t comma = inner.find(',');
            std::string elemStr = comma == std::string::npos ? inner : inner.substr(0, comma);
            int len = 0;
            if (comma != std::string::npos)
                len = std::atoi(trimCopy(inner.substr(comma + 1)).c_str());
            return llvm::ArrayType::get(mapType(trimCopy(elemStr)), len > 0 ? len : 0);
        }
        // 函数指针 (p1, p2)->ret → 不透明指针（opaque pointer，8 字节槽位）
        if (!ty.empty() && ty[0] == '(' && ty.find(")->") != std::string::npos)
            return llvm::PointerType::get(c, 0);
        // 未知类型按 i32 兜底（概念阶段；随迁移逐步精确化）
        return llvm::Type::getInt32Ty(c);
    }

    // ---- LLVM 文本类型 → Type（内建函数签名表用）------------------------------
    llvm::Type *mapLLVMType(const std::string &t)
    {
        auto &c = *ctx_;
        if (t == "i1") return llvm::Type::getInt1Ty(c);
        if (t == "i8") return llvm::Type::getInt8Ty(c);
        if (t == "i16") return llvm::Type::getInt16Ty(c);
        if (t == "i32") return llvm::Type::getInt32Ty(c);
        if (t == "i64") return llvm::Type::getInt64Ty(c);
        if (t == "float") return llvm::Type::getFloatTy(c);
        if (t == "double") return llvm::Type::getDoubleTy(c);
        if (t == "void") return llvm::Type::getVoidTy(c);
        if (t == "ptr" || t == "i8*") return llvm::PointerType::get(c, 0);
        return llvm::PointerType::get(c, 0); // 默认 opaque 指针
    }

    /** 值 → 目标类型（i32→i64 zext、i32→double/float sitofp、i32→i16/i8 trunc） */
    llvm::Value *coerceTo(llvm::Value *v, llvm::Type *target)
    {
        if (v->getType() == target) return v;
        if (v->getType()->isIntegerTy(32) && target->isIntegerTy(64))
            return builder_->CreateZExt(v, target, "zext");
        if (v->getType()->isIntegerTy(32) && target->isIntegerTy(16))
            return builder_->CreateTrunc(v, target, "trunc");
        if (v->getType()->isIntegerTy(32) && target->isIntegerTy(8))
            return builder_->CreateTrunc(v, target, "trunc");
        if (v->getType()->isIntegerTy(32) && target->isDoubleTy())
            return builder_->CreateSIToFP(v, target, "sitofp");
        if (v->getType()->isIntegerTy(32) && target->isFloatTy())
            return builder_->CreateSIToFP(v, target, "sitofp");
        return v; // 兜底：交给 verifyModule 报类型不匹配
    }

    struct BuiltinSig { const char *symbol; const char *ret; std::vector<const char *> params; };
    static const std::map<std::string, BuiltinSig> &builtinSigs()
    {
        static const std::map<std::string, BuiltinSig> m = {
            // 量子核心 / QLM
            {"encode_text", {"rvdl_encode_text", "ptr", {"ptr"}}},
            {"encode_amplitudes", {"rvdl_encode_amplitudes", "ptr", {"ptr", "i32"}}},
            {"encode_adaptive", {"rvdl_encode_adaptive", "ptr", {"ptr"}}},
            {"encode_image", {"rvdl_encode_image", "ptr", {"ptr", "i32"}}},
            {"qlm_invoke", {"rvdl_qlm_invoke", "ptr", {"ptr", "i32", "double"}}},
            {"qlm_load", {"rvdl_qlm_load", "ptr", {"ptr"}}},
            {"qlm_forward", {"rvdl_qlm_forward", "void", {"ptr", "ptr"}}},
            {"rvdl_encode_string", {"rvdl_encode_string", "ptr", {"ptr"}}},
            {"rvdl_decode_string", {"rvdl_decode_string", "ptr", {"ptr"}}},
            // 脑机接口
            {"mind_read", {"rvdl_mind_read", "ptr", {"ptr"}}},
            {"mind_train", {"rvdl_mind_train", "void", {"ptr", "i32", "double"}}},
            {"mind_feedback", {"rvdl_mind_feedback", "void", {"ptr"}}},
            {"veda_qlm_train", {"rvdl_veda_qlm_train", "void", {"ptr", "i32", "double"}}},
            // QRC 量子储备池
            {"qrc_new", {"rvdl_qrc_new", "ptr", {"i32", "i32"}}},
            {"qrc_train", {"rvdl_qrc_train", "void", {"ptr", "i32", "double"}}},
            {"qrc_probe", {"rvdl_qrc_probe", "ptr", {"ptr", "ptr"}}},
            {"qrc_predict", {"rvdl_qrc_predict", "ptr", {"ptr", "ptr"}}},
            {"qrc_release", {"rvdl_qrc_release", "void", {"ptr"}}},
            // TQNF 拓扑量子神经场
            {"dla_dim", {"rvdl_dla_dim", "i32", {"ptr", "i32"}}},
            {"qstate_entropy", {"rvdl_qstate_entropy", "double", {"ptr"}}},
            {"qstate_fidelity", {"rvdl_qstate_fidelity", "double", {"ptr", "ptr"}}},
            {"qstate_purity", {"rvdl_qstate_purity", "double", {"ptr"}}},
            {"qstate_linear_entropy", {"rvdl_qstate_linear_entropy", "double", {"ptr"}}},
            {"qstate_renyi_entropy", {"rvdl_qstate_renyi_entropy", "double", {"ptr", "double"}}},
            {"math_dot", {"rvdl_math_dot", "double", {"ptr", "ptr", "i32"}}},
            {"math_norm", {"rvdl_math_norm", "double", {"ptr", "i32"}}},
            {"qattention", {"rvdl_qattention", "double", {"ptr", "ptr"}}},
            {"geodesic_distance", {"rvdl_geodesic_distance", "double", {"ptr", "ptr"}}},
            {"inversion", {"rvdl_inversion", "double", {"double"}}},
            {"hyperbolic_metric", {"rvdl_hyperbolic_metric", "double", {"double"}}},
            {"hyperbolic_distance", {"rvdl_hyperbolic_distance", "double", {"double", "double"}}},
            {"shannon4", {"rvdl_shannon4", "double", {"i32", "i32", "i32", "i32"}}},
            {"shannon8", {"rvdl_shannon8", "double", {"i32", "i32", "i32", "i32", "i32", "i32", "i32", "i32"}}},
            {"qgate_h", {"rvdl_qgate_h", "void", {"ptr", "i32"}}},
            {"qgate_x", {"rvdl_qgate_x", "void", {"ptr", "i32"}}},
            {"qgate_rz", {"rvdl_qgate_rz", "void", {"ptr", "i32", "double"}}},
            {"qgate_cnot", {"rvdl_qgate_cnot", "void", {"ptr", "i32", "i32"}}},
            {"qexpect_z", {"rvdl_qexpect_z", "double", {"ptr", "i32"}}},
            {"qmeasure", {"rvdl_qmeasure", "i32", {"ptr", "i32"}}},
            // 量子通道逆因果容量（回程能力）
            {"retrocausal_imax", {"rvdl_retrocausal_imax", "double", {"i32", "double"}}},
            {"retrocausal_idoe", {"rvdl_retrocausal_idoe", "double", {"i32", "double"}}},
            {"retrocausal_q_capacity", {"rvdl_retrocausal_q_capacity", "double", {"i32", "double"}}},
            {"retrocausal_c_capacity", {"rvdl_retrocausal_c_capacity", "double", {"i32", "double"}}},
            {"retrocausal_q_one_shot", {"rvdl_retrocausal_q_one_shot", "double", {"i32", "double", "double"}}},
            {"retrocausal_gain", {"rvdl_retrocausal_gain", "double", {"i32", "double"}}},
            {"retrocausal_deformed", {"rvdl_retrocausal_deformed", "double", {"i32", "double", "double"}}},
            // 快子 KK 双空间嘈杂 CTC 通信（n 个 DD 维度）
            {"retrocausal_ctc_q_capacity", {"rvdl_retrocausal_ctc_q_capacity", "double", {"i32", "double", "double", "double"}}},
            {"retrocausal_ctc_c_capacity", {"rvdl_retrocausal_ctc_c_capacity", "double", {"i32", "double", "double", "double"}}},
            {"retrocausal_ctc_gain", {"rvdl_retrocausal_ctc_gain", "double", {"i32", "double", "double", "double"}}},
            {"retrocausal_ctc_dephasing", {"rvdl_retrocausal_ctc_dephasing", "double", {"double"}}},
            // 神经 / 软逻辑原语
            {"surrogate", {"rvdl_surrogate", "double", {"double", "double", "double"}}},
            {"tanh_quantize", {"rvdl_tanh_quantize", "double", {"double", "double", "i32"}}},
            {"lif_step", {"rvdl_lif_step", "double", {"double", "double", "double", "double"}}},
            {"mellowmax2", {"rvdl_mellowmax2", "double", {"double", "double", "double"}}},
            {"logsumexp2", {"rvdl_logsumexp2", "double", {"double", "double", "double"}}},
            {"boltzmann2", {"rvdl_boltzmann2", "double", {"double", "double", "double"}}},
            {"tnorm_luk", {"rvdl_tnorm_luk", "double", {"double", "double"}}},
            {"tnorm_prod", {"rvdl_tnorm_prod", "double", {"double", "double"}}},
            {"tnorm_godel", {"rvdl_tnorm_godel", "double", {"double", "double"}}},
            {"polymer_weight", {"rvdl_polymer_weight", "double", {"double", "double", "double"}}},
            {"polymer_mix_bound", {"rvdl_polymer_mix_bound", "double", {"double", "double"}}},
            // QCOS syscall
            {"rvdl_sys_call", {"rvdl_sys_call", "i32", {"i32", "i32", "i32", "i32"}}},
            {"rvdl_sys_calld", {"rvdl_sys_calld", "double", {"i32", "double", "double"}}},
            {"rvdl_sys_log", {"rvdl_sys_log", "void", {"i32", "ptr"}}},
            {"rvdl_sys_logi", {"rvdl_sys_logi", "i32", {"i32", "i32"}}},
            {"rvdl_sys_callp", {"rvdl_sys_callp", "ptr", {"i32", "i64", "i64", "i64"}}},
            {"rvdl_gc_alloc", {"rvdl_gc_alloc", "ptr", {"i64"}}},
            {"rvdl_gc_free", {"rvdl_gc_free", "void", {"ptr"}}},
            // 字符串遍历 / 内核全局状态区
            {"strlen", {"strlen", "i32", {"ptr"}}},
            {"rvdl_str_index", {"rvdl_str_index", "i8", {"ptr", "i32"}}},
            {"kglobals_addr", {"kglobals_addr", "i32", {}}},
            // QMS 数值内核
            {"rvdl_qms_gap", {"rvdl_qms_gap", "double", {"i32", "double", "double"}}},
            {"rvdl_mix_bound", {"rvdl_mix_bound", "double", {"double", "double", "double"}}},
            {"rvdl_qms_conc", {"rvdl_qms_conc", "double", {"double", "double"}}},
            // QChain 量子区块链
            {"qchain_wallet", {"rvdl_qchain_wallet", "ptr", {}}},
            {"qchain_balance", {"rvdl_qchain_balance", "i64", {"ptr"}}},
            {"qchain_mint", {"rvdl_qchain_mint", "void", {"ptr", "i64"}}},
            {"qchain_transfer", {"rvdl_qchain_transfer", "i32", {"ptr", "ptr", "i64"}}},
            {"qchain_mine", {"rvdl_qchain_mine", "i32", {}}},
            {"qchain_height", {"rvdl_qchain_height", "i32", {}}},
            {"qchain_verify", {"rvdl_qchain_verify", "i32", {}}},
            {"qchain_qkd", {"rvdl_qchain_qkd", "ptr", {"i32"}}},
            {"qchain_qdba", {"rvdl_qchain_qdba", "i32", {"i32"}}},
            {"qchain_coin_mint", {"rvdl_qchain_coin_mint", "ptr", {"i32"}}},
            {"qchain_coin_verify", {"rvdl_qchain_coin_verify", "i32", {"ptr"}}},
            {"qchain_sha3", {"rvdl_qchain_sha3", "ptr", {"ptr"}}},
            {"qchain_hmac", {"rvdl_qchain_hmac", "ptr", {"ptr", "ptr"}}},
            {"qchain_hash_unicode", {"rvdl_qchain_hash_unicode", "ptr", {"ptr"}}},
            {"qchain_sign", {"rvdl_qchain_sign", "ptr", {"ptr"}}},
            {"qchain_sign_verify", {"rvdl_qchain_sign_verify", "i32", {"ptr", "ptr"}}},
            {"qchain_sign_pubkey", {"rvdl_qchain_sign_pubkey", "ptr", {}}},
            {"qchain_mlkem_encaps", {"rvdl_qchain_mlkem_encaps", "ptr", {"ptr"}}},
            {"qchain_mlkem_decaps", {"rvdl_qchain_mlkem_decaps", "ptr", {"ptr", "ptr"}}},
            {"qchain_causal_verify", {"rvdl_qchain_causal_verify", "i32", {}}},
            {"qchain_cipher_encrypt", {"rvdl_qchain_cipher_encrypt", "ptr", {"i64", "ptr"}}},
            {"qchain_cipher_decrypt", {"rvdl_qchain_cipher_decrypt", "ptr", {"i64", "ptr"}}},
            // 晶格内省 + 访问
            {"lattice_rank", {"rvdl_lattice_rank", "i32", {"ptr"}}},
            {"lattice_size", {"rvdl_lattice_size", "i32", {"ptr", "i32"}}},
            {"lattice_boundary", {"rvdl_lattice_boundary", "i32", {"ptr"}}},
            {"rvdl_lattice_ref", {"rvdl_lattice_ref", "i32", {"ptr", "i32", "i32"}}},
            {"rvdl_lattice_new", {"rvdl_lattice_new", "ptr", {"i32", "i32", "i32", "i32"}}},
            {"rvdl_lattice_free", {"rvdl_lattice_free", "void", {"ptr"}}},
            {"rvdl_lattice_set", {"rvdl_lattice_set", "void", {"ptr", "i32", "i32", "i32"}}},
            // 量子对象构造 / 测量 / 释放（普通 declare+call）
            {"rvdl_create_DiracState", {"rvdl_create_DiracState", "ptr", {"i32"}}},
            {"rvdl_create_BellState", {"rvdl_create_BellState", "ptr", {}}},
            {"rvdl_create_QuantumRegister", {"rvdl_create_QuantumRegister", "ptr", {"i32"}}},
            {"rvdl_create_basis_state", {"rvdl_create_basis_state", "ptr", {"double", "double", "i32"}}},
            {"rvdl_measure_object", {"rvdl_measure_object", "i32", {"ptr"}}},
            {"rvdl_extract_qubit", {"rvdl_extract_qubit", "ptr", {"ptr", "i32"}}},
            {"rvdl_release_object", {"rvdl_release_object", "void", {"ptr"}}},
            {"rvdl_qkm_export", {"rvdl_qkm_export", "void", {"ptr", "ptr"}}},
            // 经典 GUI（cgui_*）—— rvdl_cgui_* ABI
            {"cgui_init", {"rvdl_cgui_init", "i32", {"i32", "i32", "ptr"}}},
            {"cgui_should_close", {"rvdl_cgui_should_close", "i32", {}}},
            {"cgui_begin_frame", {"rvdl_cgui_begin_frame", "void", {}}},
            {"cgui_end_frame", {"rvdl_cgui_end_frame", "void", {}}},
            {"cgui_button", {"rvdl_cgui_button", "i32", {"ptr"}}},
            {"cgui_text", {"rvdl_cgui_text", "void", {"ptr"}}},
            {"cgui_text_int", {"rvdl_cgui_text_int", "void", {"i32"}}},
            {"cgui_beep", {"rvdl_cgui_beep", "void", {"i32", "i32"}}},
            {"cgui_width", {"rvdl_cgui_width", "i32", {}}},
            {"cgui_height", {"rvdl_cgui_height", "i32", {}}},
            {"cgui_panel", {"rvdl_cgui_panel", "void", {"i32", "i32", "i32", "i32", "ptr"}}},
            {"cgui_panel_end", {"rvdl_cgui_panel_end", "void", {}}},
            {"cgui_row", {"rvdl_cgui_row", "void", {"i32", "i32"}}},
            {"cgui_mouse_x", {"rvdl_cgui_mouse_x", "i32", {}}},
            {"cgui_mouse_y", {"rvdl_cgui_mouse_y", "i32", {}}},
            {"cgui_mouse_left_clicked", {"rvdl_cgui_mouse_left_clicked", "i32", {}}},
            // 经典图形引擎（cgfx_*）—— rvdl_cgfx_* ABI
            {"cgfx_rect", {"rvdl_cgfx_rect", "void", {"i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_line", {"rvdl_cgfx_line", "void", {"i32", "i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_polyline", {"rvdl_cgfx_polyline", "void", {"ptr", "i32", "i32", "i32"}}},
            {"cgfx_ellipse", {"rvdl_cgfx_ellipse", "void", {"i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_triangle", {"rvdl_cgfx_triangle", "void", {"i32", "i32", "i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_rect_a", {"rvdl_cgfx_rect_a", "void", {"i32", "i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_line_a", {"rvdl_cgfx_line_a", "void", {"i32", "i32", "i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_polyline_a", {"rvdl_cgfx_polyline_a", "void", {"ptr", "i32", "i32", "i32", "i32"}}},
            {"cgfx_ellipse_a", {"rvdl_cgfx_ellipse_a", "void", {"i32", "i32", "i32", "i32", "i32", "i32"}}},
            {"cgfx_triangle_a", {"rvdl_cgfx_triangle_a", "void", {"i32", "i32", "i32", "i32", "i32", "i32", "i32", "i32"}}},

            // ─── 量子图形引擎 qgfx_*（PEQG：显式投影的量子图形学）───
            // 帧生命周期：句柄是 u64（高位编码世代号），失效句柄安全返回 0/null。
            {"qgfx_frame_new", {"rvdl_qgfx_frame_new", "i64", {}}},
            {"qgfx_frame_capture", {"rvdl_qgfx_frame_capture", "i32", {"i64", "ptr", "i32"}}},
            {"qgfx_frame_set_amplitudes", {"rvdl_qgfx_frame_set_amplitudes", "void", {"i64", "ptr", "i32", "i32"}}},
            {"qgfx_frame_release", {"rvdl_qgfx_frame_release", "void", {"i64"}}},
            // 诊断 / 投影内省（P1）
            {"qgfx_frame_residual", {"rvdl_qgfx_frame_residual", "double", {"i64"}}},
            {"qgfx_frame_num_qubits", {"rvdl_qgfx_frame_num_qubits", "i32", {"i64"}}},
            {"qgfx_frame_basis_size", {"rvdl_qgfx_frame_basis_size", "i32", {"i64"}}},
            {"qgfx_projection_name", {"rvdl_qgfx_projection_name", "ptr", {"i32"}}},
            {"qgfx_projection_source_dims", {"rvdl_qgfx_projection_source_dims", "double", {"i32"}}},
            {"qgfx_projection_target_dims", {"rvdl_qgfx_projection_target_dims", "double", {"i32", "i32"}}},
            // 谓词（P2）：O(1) 代数判据，UI 可直接读数
            {"qgfx_concurrence", {"rvdl_qgfx_concurrence", "double", {"i64"}}},
            {"qgfx_quaternionic", {"rvdl_qgfx_quaternionic", "i32", {"i64", "ptr"}}},
            {"qgfx_entropy", {"rvdl_qgfx_entropy", "double", {"i64", "i32", "i32", "i32"}}},
            {"qgfx_barcode", {"rvdl_qgfx_barcode", "i32", {"i64", "i32", "i32", "ptr", "i32"}}},
            {"qgfx_same_class", {"rvdl_qgfx_same_class", "i32", {"i64", "i64", "i32", "i32", "double"}}},
            // 复值精确幅度图元（P3）
            {"qgfx_amplitude", {"rvdl_qgfx_amplitude", "double", {"i64", "i32", "i32"}}},
            {"qgfx_amplitude_phase", {"rvdl_qgfx_amplitude_phase", "double", {"i64", "i32"}}},
            {"qgfx_interference", {"rvdl_qgfx_interference", "double", {"i64", "i32", "i32"}}},
            {"qgfx_phase_hue", {"rvdl_qgfx_phase_hue", "double", {"double"}}},
            // 嵌套层级（P4）
            {"qgfx_nested_shells", {"rvdl_qgfx_nested_shells", "i32", {"i32"}}},
            {"qgfx_nested_mass", {"rvdl_qgfx_nested_mass", "double", {"i64", "i32", "i32"}}},
            {"qgfx_bloch", {"rvdl_qgfx_bloch", "void", {"i64", "i32", "ptr"}}},
            {"qgfx_probability", {"rvdl_qgfx_probability", "double", {"i64", "i32"}}},

            // ─── 量子 UI 库 qqui_*（PCQW：谓词耦合的量子控件）──────
            {"qqui_qubit_glyph", {"rvdl_qqui_qubit_glyph", "void", {"i32", "i32", "i32", "i64", "i32", "i32", "i32"}}},
            {"qqui_qubit_row", {"rvdl_qqui_qubit_row", "void", {"i32", "i32", "i32", "i64", "i32", "i32", "i32", "i32"}}},
            {"qqui_entangle_link", {"rvdl_qqui_entangle_link", "double", {"i32", "i32", "i32", "i32", "i64", "i32", "i32", "i32", "i32"}}},
            {"qqui_uncertainty_bin", {"rvdl_qqui_uncertainty_bin", "void", {"i32", "i32", "i32", "double", "double", "i32", "i32"}}},
            {"qqui_wigner_slice", {"rvdl_qqui_wigner_slice", "void", {"i32", "i32", "i32", "i32", "i64", "i32", "i32"}}},
            {"qqui_nested_bar", {"rvdl_qqui_nested_bar", "void", {"i32", "i32", "i32", "i64", "i32", "i32"}}},
            {"qqui_amplitude_strip", {"rvdl_qqui_amplitude_strip", "i32", {"i32", "i32", "i32", "i32", "i64", "i32", "i32"}}},
            {"qqui_state_card", {"rvdl_qqui_state_card", "void", {"i32", "i32", "i32", "i64", "i32", "i32", "i32"}}},
        };
        return m;
    }

    struct GateSig { const char *symbol; const char *ret; std::vector<const char *> params; };
    /** QIR 量子门映射（QGate 的 gate 名 → QIR 符号 + 签名） */
    static const std::map<std::string, GateSig> &gateSigs()
    {
        static const std::map<std::string, GateSig> m = {
            {"h", {"__quantum__qis__h", "void", {"ptr"}}},
            {"x", {"__quantum__qis__x", "void", {"ptr"}}},
            {"y", {"__quantum__qis__y", "void", {"ptr"}}},
            {"z", {"__quantum__qis__z", "void", {"ptr"}}},
            {"s", {"__quantum__qis__s", "void", {"ptr"}}},
            {"t", {"__quantum__qis__t", "void", {"ptr"}}},
            {"rz", {"__quantum__qis__rz", "void", {"double", "ptr"}}},
            {"rx", {"__quantum__qis__rx", "void", {"double", "ptr"}}},
            {"ry", {"__quantum__qis__ry", "void", {"double", "ptr"}}},
            {"cnot", {"__quantum__qis__cnot", "void", {"ptr", "ptr"}}},
            {"toffoli", {"__quantum__qis__toffoli", "void", {"ptr", "ptr", "ptr"}}},
            {"swap", {"__quantum__qis__swap", "void", {"ptr", "ptr"}}},
            {"qft", {"__quantum__qis__qft", "void", {"i32"}}},
            {"iqft", {"__quantum__qis__iqft", "void", {"i32"}}},
            {"cqft", {"__quantum__qis__cqft", "void", {"ptr", "i32"}}},
            {"braid", {"__quantum__qis__braid", "void", {"ptr", "ptr"}}},
            {"cbraid", {"__quantum__qis__cbraid", "void", {"ptr", "ptr", "ptr"}}},
            {"measure_basis", {"__quantum__qis__measure_basis", "i32", {"ptr", "i8"}}},
            // 受控门（可逆编织 @[steer]）
            {"cx", {"__quantum__qis__cx", "void", {"ptr", "ptr"}}},
            {"ch", {"__quantum__qis__ch", "void", {"ptr", "ptr"}}},
            {"crz", {"__quantum__qis__crz", "void", {"ptr", "ptr", "double"}}},
            {"cswap", {"__quantum__qis__cswap", "void", {"ptr", "ptr", "ptr"}}},
            {"c_toffoli", {"__quantum__qis__c_toffoli", "void", {"ptr", "ptr", "ptr", "ptr"}}},
        };
        return m;
    }

    /** 特殊内建：原子 / 端口 I/O / 地址（非 declare+call，生成 LLVM 特殊指令） */
    llvm::Value *buildSpecialCall(const json::Value &call, const std::string &target)
    {
        const auto &args = call.at("args").array();
        if (target == "sync_load")
        {
            llvm::Value *p = buildOperand(args[0]);
            llvm::LoadInst *li = builder_->CreateLoad(llvm::Type::getInt32Ty(*ctx_), p, "atomicload");
            li->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
            return li;
        }
        if (target == "sync_store")
        {
            llvm::Value *p = buildOperand(args[0]);
            llvm::Value *v = buildOperand(args[1]);
            llvm::StoreInst *si = builder_->CreateStore(v, p);
            si->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
            return si;
        }
        if (target == "sync_add")
        {
            llvm::Value *p = buildOperand(args[0]);
            llvm::Value *v = buildOperand(args[1]);
            return builder_->CreateAtomicRMW(llvm::AtomicRMWInst::Add, p, v, llvm::MaybeAlign(4),
                                             llvm::AtomicOrdering::SequentiallyConsistent);
        }
        if (target == "sync_cas")
        {
            llvm::Value *p = buildOperand(args[0]);
            llvm::Value *oldV = buildOperand(args[1]);
            llvm::Value *newV = buildOperand(args[2]);
            llvm::AtomicCmpXchgInst *cx = builder_->CreateAtomicCmpXchg(
                p, oldV, newV, llvm::MaybeAlign(4),
                llvm::AtomicOrdering::SequentiallyConsistent, llvm::AtomicOrdering::SequentiallyConsistent);
            return builder_->CreateExtractValue(cx, 0, "casval");
        }
        if (target == "sync_lock")
        {
            // 锁原语：原子 TAS（atomicrmw xchg 0 -> 1）
            llvm::Value *p = buildOperand(args[0]);
            return builder_->CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, p,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 1), llvm::MaybeAlign(4),
                llvm::AtomicOrdering::SequentiallyConsistent);
        }
        if (target == "sync_unlock")
        {
            // 锁释放：store atomic 0
            llvm::Value *p = buildOperand(args[0]);
            llvm::StoreInst *si = builder_->CreateStore(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0), p);
            si->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
            return si;
        }
        if (target == "addr")
        {
            llvm::Value *p = buildOperand(args[0]);
            return builder_->CreatePtrToInt(p, llvm::Type::getInt32Ty(*ctx_), "addr");
        }
        if (target == "outb")
        {
            // outb(port, value)：port=i16(dx)、value=i8(ax)
            llvm::Value *port = coerceTo(buildOperand(args[0]), llvm::Type::getInt16Ty(*ctx_));
            llvm::Value *value = coerceTo(buildOperand(args[1]), llvm::Type::getInt8Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                {llvm::Type::getInt8Ty(*ctx_), llvm::Type::getInt16Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "outb ${0:b}, ${1:w}",
                "{ax},{dx},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {value, port});
        }
        if (target == "inb")
        {
            // inb(port)：port=i16(dx)，返回 i8(al) 后 zext 到 i32
            llvm::Value *port = coerceTo(buildOperand(args[0]), llvm::Type::getInt16Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getInt8Ty(*ctx_),
                {llvm::Type::getInt16Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "inb ${1:w}, ${0:b}",
                "={ax},{dx},~{dirflag},~{fpsr},~{flags}", true);
            llvm::Value *res = builder_->CreateCall(ia, {port}, "inb");
            return builder_->CreateZExt(res, llvm::Type::getInt32Ty(*ctx_), "inbzext");
        }
        // 16 位端口 I/O（PCI/ATA 底层）
        if (target == "outw")
        {
            llvm::Value *port = coerceTo(buildOperand(args[0]), llvm::Type::getInt16Ty(*ctx_));
            llvm::Value *value = coerceTo(buildOperand(args[1]), llvm::Type::getInt16Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                {llvm::Type::getInt16Ty(*ctx_), llvm::Type::getInt16Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "outw ${0:w}, ${1:w}",
                "{ax},{dx},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {value, port});
        }
        if (target == "inw")
        {
            llvm::Value *port = coerceTo(buildOperand(args[0]), llvm::Type::getInt16Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getInt16Ty(*ctx_),
                {llvm::Type::getInt16Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "inw ${1:w}, ${0:w}",
                "={ax},{dx},~{dirflag},~{fpsr},~{flags}", true);
            llvm::Value *res = builder_->CreateCall(ia, {port}, "inw");
            return builder_->CreateZExt(res, llvm::Type::getInt32Ty(*ctx_), "inwzext");
        }
        // 32 位端口 I/O（PCI 配置空间等）
        if (target == "outl")
        {
            llvm::Value *port = coerceTo(buildOperand(args[0]), llvm::Type::getInt16Ty(*ctx_));
            llvm::Value *value = coerceTo(buildOperand(args[1]), llvm::Type::getInt32Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                {llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt16Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "outl ${0:k}, ${1:w}",
                "{eax},{dx},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {value, port});
        }
        if (target == "inl")
        {
            llvm::Value *port = coerceTo(buildOperand(args[0]), llvm::Type::getInt16Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx_),
                {llvm::Type::getInt16Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "inl ${1:w}, ${0:k}",
                "={eax},{dx},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {port}, "inl");
        }
        // volatile 内存访问（MMIO 轮询不被 -O2 优化为死循环）
        if (target == "volatile_load")
        {
            llvm::Value *p = buildOperand(args[0]);
            llvm::LoadInst *li = builder_->CreateLoad(llvm::Type::getInt32Ty(*ctx_), p, "volload");
            li->setVolatile(true);
            return li;
        }
        if (target == "volatile_store")
        {
            llvm::Value *p = buildOperand(args[0]);
            llvm::Value *v = coerceTo(buildOperand(args[1]), llvm::Type::getInt32Ty(*ctx_));
            llvm::StoreInst *si = builder_->CreateStore(v, p);
            si->setVolatile(true);
            return si;
        }
        // 读控制寄存器：mov %crN, ${0}（i64 输出）
        if (target == "read_cr0" || target == "read_cr2" || target == "read_cr3" || target == "read_cr4")
        {
            std::string cr = target.substr(5); // "cr0"/"cr2"/"cr3"/"cr4"
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getInt64Ty(*ctx_), {}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "mov ${0}, %" + cr,
                "=r,~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {}, target);
        }
        // 写控制寄存器：mov %crN, ${0}（void）
        if (target == "write_cr0" || target == "write_cr3")
        {
            std::string cr = target.substr(6); // "cr0"/"cr3"
            llvm::Value *v = coerceTo(buildOperand(args[0]), llvm::Type::getInt64Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                {llvm::Type::getInt64Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "mov %" + cr + ", ${0}",
                "r,~{memory},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {v}, target);
        }
        if (target == "invlpg")
        {
            llvm::Value *a = coerceTo(buildOperand(args[0]), llvm::Type::getInt64Ty(*ctx_));
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                {llvm::Type::getInt64Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "invlpg (${0})",
                "r,~{memory},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {a}, "invlpg");
        }
        // rdmsr / rdtsc / xgetbv：EDX:EAX 双输出，组合为 i64（x86-64 下 =A 不再表示 edx:eax 对）
        if (target == "rdmsr" || target == "rdtsc" || target == "xgetbv")
        {
            std::vector<llvm::Type *> in_types;
            std::vector<llvm::Value *> in_vals;
            std::string tpl;
            if (target == "rdmsr")
            {
                in_types.push_back(llvm::Type::getInt32Ty(*ctx_));
                in_vals.push_back(coerceTo(buildOperand(args[0]), llvm::Type::getInt32Ty(*ctx_)));
                tpl = "rdmsr";
            }
            else if (target == "xgetbv")
            {
                in_types.push_back(llvm::Type::getInt32Ty(*ctx_));
                in_vals.push_back(coerceTo(buildOperand(args[0]), llvm::Type::getInt32Ty(*ctx_)));
                tpl = "xgetbv";
            }
            else
            {
                tpl = "rdtsc";
            }
            llvm::StructType *st = llvm::StructType::get(*ctx_, {llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt32Ty(*ctx_)});
            llvm::FunctionType *ft = llvm::FunctionType::get(st, in_types, false);
            std::string cons = "={eax},={edx}";
            if (target == "rdmsr" || target == "xgetbv") cons += ",{ecx},~{memory}";
            cons += ",~{dirflag},~{fpsr},~{flags}";
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, tpl, cons, true);
            llvm::Value *res = builder_->CreateCall(ia, in_vals, target);
            llvm::Value *lo = builder_->CreateExtractValue(res, 0, "lo");
            llvm::Value *hi = builder_->CreateExtractValue(res, 1, "hi");
            llvm::Value *zlo = builder_->CreateZExt(lo, llvm::Type::getInt64Ty(*ctx_), "zlo");
            llvm::Value *zhi = builder_->CreateZExt(hi, llvm::Type::getInt64Ty(*ctx_), "zhi");
            llvm::Value *sh = builder_->CreateShl(zhi, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 32), "sh");
            return builder_->CreateOr(zlo, sh, "comb");
        }
        if (target == "wrmsr")
        {
            llvm::Value *msr = coerceTo(buildOperand(args[0]), llvm::Type::getInt32Ty(*ctx_));
            llvm::Value *v = coerceTo(buildOperand(args[1]), llvm::Type::getInt64Ty(*ctx_));
            llvm::Value *lo = builder_->CreateTrunc(v, llvm::Type::getInt32Ty(*ctx_), "lo");
            llvm::Value *hi = builder_->CreateTrunc(
                builder_->CreateLShr(v, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 32), "hish"),
                llvm::Type::getInt32Ty(*ctx_), "hi");
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                {llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt32Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "wrmsr",
                "{ecx},{eax},{edx},~{memory},~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {msr, lo, hi}, "wrmsr");
        }
        if (target == "cpuid")
        {
            llvm::Value *leaf = coerceTo(buildOperand(args[0]), llvm::Type::getInt32Ty(*ctx_));
            llvm::Value *sub = coerceTo(buildOperand(args[1]), llvm::Type::getInt32Ty(*ctx_));
            llvm::StructType *st = llvm::StructType::get(*ctx_,
                {llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt32Ty(*ctx_),
                 llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt32Ty(*ctx_)});
            llvm::FunctionType *ft = llvm::FunctionType::get(st,
                {llvm::Type::getInt32Ty(*ctx_), llvm::Type::getInt32Ty(*ctx_)}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "cpuid",
                "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory},~{dirflag},~{fpsr},~{flags}", true);
            llvm::Value *res = builder_->CreateCall(ia, {leaf, sub}, "cpuid");
            llvm::Value *last = nullptr;
            for (unsigned i = 0; i < 4; ++i)
            {
                llvm::Value *ext = builder_->CreateExtractValue(res, i, "cpuidreg");
                llvm::Value *ptr = buildOperand(args[2 + i]);
                last = builder_->CreateStore(ext, ptr);
            }
            return last; // void（store 指令）
        }
        if (target == "read_rflags")
        {
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getInt64Ty(*ctx_), {}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, "pushfq; pop ${0}",
                "=r,~{dirflag},~{fpsr},~{flags}", true);
            return builder_->CreateCall(ia, {}, "rflags");
        }
        return nullptr;
    }

    // ---- 拓扑入口（@layer 多维标签函数调度）--------------------------------
    //
    // 从 MIR 的 layer 标签聚合调度表，生成 @rvdl_topology_json 常量与
    // @rvdl_topology_entry 函数（调用 rvdl_runtime_run_topology）。
    //
    // ⚠ 原实现只序列化 name/time/thread 并把 coord 硬编码为 [1]，**丢弃**
    //   cost / deadline / coord 维度 / callGraph。本实现补齐全部字段，
    //   并按 server/src/topology.ts 的同款规则重新推导 shuttleLinks /
    //   trackingScopes —— 否则 `--mir` 下沉路径会**静默丢失**穿梭与追踪信息
    //   （TS 侧已报的错误会阻断执行，故此处只序列化已通过校验的输入）。
    //
    // 诊断（E-SHU* / E-TRK* / E-TOP008）由 server 侧 TopologyBuilder 负责；
    // 本函数不重复报错，只对已合法数据做纯序列化 + 权重计算。
    void emitTopologyEntry(const json::Value &doc)
    {
        // 可观测量族 → 保真级别（须与 server/src/topology.ts 的 TRACKABLE_OBSERVABLES 一致）
        // 0 = L0/qnd（Pauli 族，非破坏，Peierls）  1 = L1/shadow（需测量，统计）
        // 2 = L2/relabel（纯经典量，无需抽取电路）
        static const std::map<std::string, int> kObservableLevel = {
            {"expect_z", 0}, {"expect_x", 0}, {"expect_y", 0},
            {"num_qubits", 2}, {"shannon4", 2}, {"shannon8", 2}, {"amplitude", 2},
            {"entropy", 1}, {"fidelity", 1}, {"attention", 1},
            {"measure_z", 1}, {"measure_x", 1}, {"measure_y", 1},
        };

        struct LayerInfo
        {
            std::string name;
            int time = 0;
            int thread = 0;
            std::vector<int> coord;
            int cost = 1;
            bool has_deadline = false;
            int deadline = 0;
            bool dual = false;
            // 穿梭
            bool has_shuttle = false;
            std::vector<std::vector<int>> shuttle_targets;
            double shuttle_lambda = 1.0;
            int shuttle_phase = 2;
            int shuttle_rounds = 0;   // 0 = auto
            int shuttle_strategy = -1; // -1 = auto
            // 追踪
            bool has_track = false;
            std::vector<std::string> track_observables;
            int track_mode = 0;
            int track_slots = 1;
        };

        std::vector<LayerInfo> layers;
        int maxTime = 0, maxThread = 0, maxCoordDim = 0;
        for (const auto &bv : doc.at("bodies").array())
        {
            if (!bv.has("layer") || bv.at("layer").is_null())
                continue;
            // 入口约定无参数；有参数的 @layer 函数是被调用的辅助/诊断函数，
            // 不参与拓扑独立调度（与 ir.ts buildTopologyJson 口径一致）。
            if (bv.has("params") && bv.at("params").is_array() && !bv.at("params").array().empty())
                continue;

            const auto &ly = bv.at("layer");
            LayerInfo li;
            li.name = bv.at("owner").string();
            li.time = ly.has("time") && !ly.at("time").is_null() ? ly.at("time").int_value() : 0;
            li.thread = ly.has("thread") ? ly.at("thread").int_value() : 0;
            if (ly.has("coord") && ly.at("coord").is_array())
                for (const auto &c : ly.at("coord").array())
                    li.coord.push_back(c.int_value());
            li.cost = ly.has("cost") && !ly.at("cost").is_null() ? ly.at("cost").int_value() : 1;
            li.has_deadline = ly.has("deadline") && !ly.at("deadline").is_null();
            li.deadline = li.has_deadline ? ly.at("deadline").int_value() : 0;
            li.dual = ly.has("dual") && ly.at("dual").is_bool() ? ly.at("dual").bool_value() : false;

            if (ly.has("shuttle") && !ly.at("shuttle").is_null())
            {
                const auto &sh = ly.at("shuttle");
                li.has_shuttle = true;
                if (sh.has("targets") && sh.at("targets").is_array())
                    for (const auto &t : sh.at("targets").array())
                    {
                        std::vector<int> tc;
                        if (t.is_array())
                            for (const auto &v : t.array())
                                tc.push_back(v.int_value());
                        li.shuttle_targets.push_back(std::move(tc));
                    }
                li.shuttle_lambda = sh.has("lambda") && !sh.at("lambda").is_null() ? sh.at("lambda").number() : 1.0;
                li.shuttle_phase = sh.has("phase") ? sh.at("phase").int_value() : 2;
                li.shuttle_rounds = sh.has("rounds") ? sh.at("rounds").int_value() : 0;
                li.shuttle_strategy = sh.has("strategy") ? sh.at("strategy").int_value() : -1;
            }
            if (ly.has("track") && !ly.at("track").is_null())
            {
                const auto &tr = ly.at("track");
                li.has_track = true;
                if (tr.has("observables") && tr.at("observables").is_array())
                    for (const auto &o : tr.at("observables").array())
                        li.track_observables.push_back(o.string());
                li.track_mode = tr.has("mode") ? tr.at("mode").int_value() : 0;
                li.track_slots = tr.has("slots") ? tr.at("slots").int_value() : 1;
            }

            if (li.time > maxTime) maxTime = li.time;
            if (li.thread > maxThread) maxThread = li.thread;
            if ((int)li.coord.size() > maxCoordDim) maxCoordDim = (int)li.coord.size();
            layers.push_back(std::move(li));
        }
        if (layers.empty())
            return;

        // ---- shape：逐维 coord 上界 ------------------------------------
        std::vector<int> coordMax(maxCoordDim, 0);
        std::vector<int> coordMin(maxCoordDim, 0);
        std::vector<bool> coordSeen(maxCoordDim, false);
        for (const auto &li : layers)
            for (size_t i = 0; i < li.coord.size() && i < (size_t)maxCoordDim; ++i)
            {
                coordMax[i] = std::max(coordMax[i], li.coord[i]);
                coordMin[i] = coordSeen[i] ? std::min(coordMin[i], li.coord[i]) : li.coord[i];
                coordSeen[i] = true;
            }
        for (size_t i = 0; i < coordMax.size(); ++i)
            if (!coordSeen[i])
                coordMin[i] = 0;

        auto coordKey = [](const std::vector<int> &c)
        {
            std::string k;
            for (size_t i = 0; i < c.size(); ++i)
            {
                if (i) k += ',';
                k += std::to_string(c[i]);
            }
            return k;
        };
        auto pairKey = [](const std::string &a, const std::string &b)
        {
            return a < b ? a + '\x1f' + b : b + '\x1f' + a;
        };
        auto l1Distance = [](const std::vector<int> &a, const std::vector<int> &b)
        {
            size_t n = std::max(a.size(), b.size());
            int d = 0;
            for (size_t i = 0; i < n; ++i)
                d += std::abs((i < a.size() ? a[i] : 0) - (i < b.size() ? b[i] : 0));
            return d;
        };

        // ---- 邻接表：块对 → 关联边种类（与 deriveEdges 同款判定）--------
        // deriveEdges 的完备性：coord 不同的两块必得 parallel 或 projection；
        // coord 相同的按 dual/thread 得 stack / inversion / crossing。
        std::map<std::string, std::string> adj;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            for (size_t j = i + 1; j < layers.size(); ++j)
            {
                const auto &A = layers[i];
                const auto &B = layers[j];
                std::string kind;
                if (coordKey(A.coord) != coordKey(B.coord))
                    kind = (A.coord.size() != B.coord.size()) ? "projection" : "parallel";
                else
                {
                    if (A.dual == B.dual && A.thread == B.thread) kind = "stack";
                    if (A.dual != B.dual) kind = "inversion";
                    else if (A.thread != B.thread) kind = "crossing";
                }
                if (!kind.empty())
                    adj.emplace(pairKey(A.name, B.name), kind);
            }
        }

        // ---- 穿梭链接 ----------------------------------------------------
        // 轮次 auto 按**所选策略**的解析式展开（与 server/src/topology.ts 的
        // strategyDepth() 逐字同式，由 runtime/tests/shuttle_topology_parity.cpp 钉住）：
        //   蝶式 ⌈log₂span⌉ / 环式 n / 树式 ⌈log₂(n+1)⌉ / Grover 1 / c001 蝶点 1。
        // 各维 span_i = coord_max[i] − min(0, coord_min[i]) + 1。
        // coord 允许为负（格点地址），故不能直接用 coord_max+1 —— 那样全负地址
        // 会得到 span=1 → rounds=0，穿梭**静默失效**。式子在全部 coord ≥ 0 且
        // 最小值为 0 时退化为 coord_max+1（既有行为不变）。
        //
        // 统一套蝶式公式是**真实存在过的缺陷**：环式实际轮数是 n、c001 恒 1，
        // 按 ⌈log₂span⌉ 解析会让 E-SHU007 的 deadline 检查系统性低估代价，
        // 而运行期展开又各按各的算 —— 两端各自「自洽」，接起来是错的。
        long long span = 1;
        for (int i = 0; i < maxCoordDim; ++i)
        {
            const int lo = std::min(0, coordMin[i]);
            span *= (long long)(std::max(1, coordMax[i] - lo + 1));
        }
        // strategy = 声明值（-1 = auto，解析式走蝶式默认）；peerCount = 该链接对端数
        auto strategyDepth = [](int strategy, size_t peerCount, long long sp) -> int {
            switch (strategy) {
            case 1: return (int)peerCount;      // 环式：绕环一圈
            case 2: return peerCount == 0 ? 0
                         : (int)std::ceil(std::log2((double)peerCount + 1.0));  // 树式：2^d−1 ≥ n
            case 3: return peerCount == 0 ? 0 : 1;                              // Grover：O(1)
            case 4: return peerCount == 0 ? 0 : 1;                              // c001 蝶点：恒 1 轮
            default: return sp <= 1 ? 1 : (int)std::ceil(std::log2((double)sp)); // 蝶式（含 auto 默认）
            }
        };

        json::Array shuttleArr;
        for (const auto &src : layers)
        {
            if (!src.has_shuttle || src.shuttle_targets.empty())
                continue;

            struct Peer
            {
                std::string name;
                std::vector<int> coord;
                double logWeight;   // −d/λ，任意距离均有限（路由/排序用）
                double weight;      // exp(logWeight)，混合用；d/λ ≳ 746 时下溢为 0
                int distance;
                std::string via;
            };
            std::vector<Peer> peers;
            std::set<std::string> seen;
            for (const auto &target : src.shuttle_targets)
            {
                for (const auto &m : layers)
                {
                    if (m.name == src.name) continue;
                    if (coordKey(m.coord) != coordKey(target)) continue;
                    if (seen.count(m.name)) continue;
                    auto ait = adj.find(pairKey(src.name, m.name));
                    if (ait == adj.end()) continue;             // 无关联边 → 不可穿梭（TS 已报 E-SHU002）
                    if (coordKey(src.coord) == coordKey(m.coord) && src.time == m.time)
                        continue;                                // 同 coord 同 time → TS 已报 E-SHU008
                    seen.insert(m.name);
                    int d = l1Distance(src.coord, m.coord);
                    // 关联度核（对数空间）：logWeight 任意距离有限，故对端不会因
                    // 浮点下溢而从计划中消失；weight 仅用于线性混合。
                    double logW = src.shuttle_lambda > 0.0
                                      ? -(double)d / src.shuttle_lambda
                                      : 0.0;
                    double w = src.shuttle_lambda > 0.0 ? std::exp(logW) : 0.0;
                    peers.push_back({m.name, m.coord, logW, w, d, ait->second});
                }
            }
            if (peers.empty()) continue;   // TS 已报错；此处不重复产出

            // 按块名排序，保证 weights 与 targets 的对应关系确定性
            std::sort(peers.begin(), peers.end(),
                      [](const Peer &x, const Peer &y) { return x.name < y.name; });

            int rounds = src.shuttle_rounds > 0
                             ? src.shuttle_rounds
                             : strategyDepth(src.shuttle_strategy, peers.size(), span);
            int phase = src.shuttle_phase;
            json::Array peerArr;
            for (const auto &p : peers)
            {
                json::Array c;
                for (int v : p.coord) c.push_back(json::Value(v));
                json::Object o;
                o["name"] = json::Value(p.name);
                o["coord"] = json::Value(c);
                o["logWeight"] = json::Value(p.logWeight);
                o["weight"] = json::Value(p.weight);
                o["distance"] = json::Value(p.distance);
                o["via"] = json::Value(p.via);
                peerArr.push_back(json::Value(o));
            }
            json::Array slots;
            if (phase == 2) { slots.push_back(json::Value(rounds)); slots.push_back(json::Value(rounds)); }
            else if (phase == 1) { slots.push_back(json::Value(0)); slots.push_back(json::Value(rounds)); }
            else { slots.push_back(json::Value(rounds)); slots.push_back(json::Value(0)); }

            json::Object lo;
            lo["from"] = json::Value(src.name);
            lo["targets"] = json::Value(peerArr);
            lo["lambda"] = json::Value(src.shuttle_lambda);
            lo["phase"] = json::Value(phase);
            lo["rounds"] = json::Value(rounds);
            lo["strategy"] = json::Value(src.shuttle_strategy);
            lo["slotsPerPhase"] = json::Value(slots);
            shuttleArr.push_back(json::Value(lo));
        }

        // ---- 追踪作用域 --------------------------------------------------
        json::Array trackArr;
        for (const auto &src : layers)
        {
            if (!src.has_track || src.track_observables.empty()) continue;

            json::Array obsArr, lvlArr, winArr;
            for (const auto &o : src.track_observables)
            {
                obsArr.push_back(json::Value(o));
                auto lit = kObservableLevel.find(o);
                int lvl = (lit != kObservableLevel.end()) ? lit->second : 1;  // 未知 → 按最弱保证
                // 强制 qnd(1) 但观测量非 Pauli 族 → TS 已报 E-TRK003；此处按 0 降级
                if (src.track_mode == 1 && lvl != 0) lvl = 0;
                lvlArr.push_back(json::Value(lvl));
            }
            winArr.push_back(json::Value(src.time));
            winArr.push_back(json::Value(src.time + src.cost));

            json::Object so;
            so["block"] = json::Value(src.name);
            so["observables"] = json::Value(obsArr);
            so["mode"] = json::Value(src.track_mode);
            so["slots"] = json::Value(std::max(1, src.track_slots));
            so["level"] = json::Value(lvlArr);
            so["window"] = json::Value(winArr);
            trackArr.push_back(json::Value(so));
        }

        // ---- 序列化 ------------------------------------------------------
        json::Array shapeCoord;
        for (int c : coordMax) shapeCoord.push_back(json::Value(c + 1));
        json::Object shapeObj;
        shapeObj["time"] = json::Value(maxTime + 1);
        shapeObj["thread"] = json::Value(maxThread + 1);
        shapeObj["coord"] = json::Value(shapeCoord);

        json::Array blockArr;
        for (const auto &li : layers)
        {
            json::Array c;
            for (int v : li.coord) c.push_back(json::Value(v));
            json::Object bo;
            bo["name"] = json::Value(li.name);
            bo["time"] = json::Value(li.time);
            bo["thread"] = json::Value(li.thread);
            bo["coord"] = json::Value(c);
            bo["cost"] = json::Value(li.cost);
            bo["deadline"] = li.has_deadline ? json::Value(li.deadline) : json::Value();
            blockArr.push_back(json::Value(bo));
        }

        json::Object root;
        root["shape"] = json::Value(shapeObj);
        root["blocks"] = json::Value(blockArr);
        // callGraph：MIR 下沉路径不重建逻辑时钟（其 Δt 语义由函数体内 CPU 顺序执行保证），
        // 故显式置空数组而非省略 —— 运行期据此区分「入口块」与「被调用子块」。
        root["callGraph"] = json::Value(json::Array{});
        root["shuttleLinks"] = json::Value(shuttleArr);
        root["trackingScopes"] = json::Value(trackArr);

        std::string json_text = json::serialize(json::Value(root));

        llvm::Constant *str = llvm::ConstantDataArray::getString(*ctx_, json_text, true);
        auto *gv = new llvm::GlobalVariable(*module_, str->getType(), true,
                                            llvm::GlobalValue::PrivateLinkage, str, "rvdl_topology_json");

        llvm::Function *entry = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx_), false),
            llvm::Function::ExternalLinkage, "rvdl_topology_entry", module_.get());
        llvm::BasicBlock *bb = llvm::BasicBlock::Create(*ctx_, "entry", entry);
        builder_->SetInsertPoint(bb);

        llvm::Function *run = declareFn("rvdl_runtime_run_topology",
                                        llvm::Type::getInt32Ty(*ctx_), {llvm::PointerType::get(*ctx_, 0)});
        llvm::Value *ptr = builder_->CreatePointerCast(gv, llvm::PointerType::get(*ctx_, 0), "jsonptr");
        llvm::Value *ret = builder_->CreateCall(run, {ptr}, "ret");
        builder_->CreateRet(ret);
    }

    // ---- 函数体构建 ---------------------------------------------------------
    void buildBody(const json::Value &bv)
    {
        const std::string owner = bv.at("owner").string();
        llvm::Function *fn = funcs_.at(owner);
        current_fn_ = fn;
        local_alloca_.clear();
        blocks_.clear();

        // 先创建所有基本块
        for (const auto &bb : bv.at("blocks").array())
        {
            int id = bb.at("id").int_value();
            blocks_[id] = llvm::BasicBlock::Create(*ctx_, "bb" + std::to_string(id), fn);
        }

        // entry：alloca 所有 locals（参数对应的 local 由实参 store 进来）。
        // void 槽（QGate 等无返回值 rvalue 的落点）不分配存储——alloca void 非法。
        builder_->SetInsertPoint(blocks_[0]);
        for (const auto &l : bv.at("locals").array())
        {
            int id = l.at("id").int_value();
            std::string ty = l.at("ty").string();
            llvm::Type *t = mapType(ty);
            local_types_[id] = ty;
            if (t->isVoidTy())
                continue;
            local_alloca_[id] = builder_->CreateAlloca(t, nullptr, "v" + std::to_string(id));
        }

        // 逐块逐语句填充
        for (const auto &bb : bv.at("blocks").array())
        {
            int id = bb.at("id").int_value();
            builder_->SetInsertPoint(blocks_[id]);
            for (const auto &stmt : bb.at("statements").array())
            {
                buildStatement(stmt);
            }
            if (bb.has("terminator") && !bb.at("terminator").is_null())
            {
                buildTerminator(bb.at("terminator"));
            }
        }
    }

    // ---- 语句 -----------------------------------------------------------------
    void buildStatement(const json::Value &stmt)
    {
        const std::string kind = stmt.at("kind").string();
        if (kind == "Assign")
        {
            int local = stmt.at("place").at("local").int_value();
            llvm::Value *val = buildRvalue(stmt.at("rvalue"));
            // void 型 rvalue（如 QGate 门调用）仅执行副作用，不产生值，跳过 store
            if (!val->getType()->isVoidTy())
                builder_->CreateStore(val, local_alloca_.at(local));
        }
        else if (kind == "Drop" || kind == "Consume")
        {
            // 线性类型的显式释放/消费：加载 place，按类型调用运行时释放内建。
            int local = stmt.at("place").at("local").int_value();
            auto it = local_alloca_.find(local);
            if (it == local_alloca_.end())
                return; // void 槽无存储
            llvm::Value *ptr = builder_->CreateLoad(it->second->getAllocatedType(), it->second, "dropval");
            const std::string ty = local_types_.count(local) ? local_types_.at(local) : "";
            if (ty == "Qubit")
            {
                llvm::Function *f = declareFn("__quantum__rt__qubit_release",
                                              llvm::Type::getVoidTy(*ctx_), {llvm::PointerType::get(*ctx_, 0)});
                builder_->CreateCall(f, {ptr});
            }
            else if (ty == "QObject" || ty.rfind("QModel", 0) == 0 || ty.rfind("QReservoir", 0) == 0)
            {
                llvm::Function *f = declareFn("rvdl_release_object",
                                              llvm::Type::getVoidTy(*ctx_), {llvm::PointerType::get(*ctx_, 0)});
                builder_->CreateCall(f, {ptr});
            }
            // 其他类型（cap/int 等）：Drop 无副作用（内存由 GC 管理）
        }
        else
        {
            throw std::runtime_error("MIR: unsupported statement kind '" + kind + "'");
        }
    }

    // ---- 右值 -----------------------------------------------------------------
    llvm::Value *buildRvalue(const json::Value &rv)
    {
        const std::string kind = rv.at("kind").string();
        if (kind == "Use")
        {
            return buildOperand(rv.at("operand"));
        }
        if (kind == "Binary")
        {
            llvm::Value *lhs = buildOperand(rv.at("lhs"));
            llvm::Value *rhs = buildOperand(rv.at("rhs"));
            const std::string op = rv.at("op").string();
            if (op == "+") return builder_->CreateAdd(lhs, rhs, "addtmp");
            if (op == "-") return builder_->CreateSub(lhs, rhs, "subtmp");
            if (op == "*") return builder_->CreateMul(lhs, rhs, "multmp");
            if (op == "/")
            {
                // 整数除法 sdiv / 浮点 fdiv 由类型决定；骨架按 lhs 类型粗判
                return lhs->getType()->isFloatingPointTy()
                           ? builder_->CreateFDiv(lhs, rhs, "divtmp")
                           : builder_->CreateSDiv(lhs, rhs, "divtmp");
            }
            if (op == "%") return builder_->CreateSRem(lhs, rhs, "remtmp");
            if (op == "==") return builder_->CreateICmpEQ(lhs, rhs, "eqtmp");
            if (op == "!=") return builder_->CreateICmpNE(lhs, rhs, "netmp");
            if (op == "<") return builder_->CreateICmpSLT(lhs, rhs, "lttmp");
            if (op == "<=") return builder_->CreateICmpSLE(lhs, rhs, "letmp");
            if (op == ">") return builder_->CreateICmpSGT(lhs, rhs, "gttmp");
            if (op == ">=") return builder_->CreateICmpSGE(lhs, rhs, "getmp");
            if (op == "|") return builder_->CreateOr(lhs, rhs, "ortmp");
            if (op == "&") return builder_->CreateAnd(lhs, rhs, "andtmp");
            if (op == "^") return builder_->CreateXor(lhs, rhs, "xortmp");
            if (op == "<<") return builder_->CreateShl(lhs, rhs, "shltmp");
            if (op == ">>") return builder_->CreateAShr(lhs, rhs, "ashrtmp");
            throw std::runtime_error("MIR: unsupported binary op '" + op + "'");
        }
        if (kind == "Unary")
        {
            llvm::Value *arg = buildOperand(rv.at("arg"));
            const std::string op = rv.at("op").string();
            if (op == "-") return builder_->CreateNeg(arg, "negtmp");
            if (op == "!") return builder_->CreateICmpEQ(arg, llvm::ConstantInt::get(arg->getType(), 0), "nottmp");
            throw std::runtime_error("MIR: unsupported unary op '" + op + "'");
        }
        if (kind == "Call")
        {
            return buildCall(rv);
        }
        if (kind == "QAlloc")
        {
            // alloc() → __quantum__rt__qubit_allocate()
            llvm::Function *f = declareFn("__quantum__rt__qubit_allocate", llvm::PointerType::get(*ctx_, 0), {});
            return builder_->CreateCall(f, {}, "qubit");
        }
        if (kind == "QMeasure")
        {
            // measure(q) → __quantum__qis__measure_int(q) : i32
            llvm::Value *arg = buildOperand(rv.at("arg"));
            llvm::Function *f = declareFn("__quantum__qis__measure_int", llvm::Type::getInt32Ty(*ctx_),
                                          {llvm::PointerType::get(*ctx_, 0)});
            return builder_->CreateCall(f, {arg}, "measure");
        }
        if (kind == "QGate")
        {
            const std::string gate = rv.at("gate").string();
            std::vector<llvm::Value *> arg_vals;
            for (const auto &a : rv.at("args").array())
                arg_vals.push_back(buildOperand(a));

            // 内建门 → QIR 内建（__quantum__qis__<gate>）
            const auto &gs = gateSigs();
            auto it = gs.find(gate);
            if (it != gs.end())
            {
                const GateSig &sig = it->second;
                std::vector<llvm::Type *> ptypes;
                for (const char *p : sig.params) ptypes.push_back(mapLLVMType(p));
                for (size_t i = 0; i < arg_vals.size() && i < ptypes.size(); ++i)
                    arg_vals[i] = coerceTo(arg_vals[i], ptypes[i]);
                llvm::Function *f = declareFn(sig.symbol, mapLLVMType(sig.ret), ptypes);
                llvm::CallInst *call = builder_->CreateCall(f, arg_vals);
                if (!call->getType()->isVoidTy())
                    call->setName("gate"); // void 指令不能有名字
                return call;
            }

            // 用户自定义门（@[gate] 函数）→ call @<gate>（借用 Qubit 参数）
            if (funcs_.count(gate))
            {
                llvm::Function *f = funcs_.at(gate);
                for (size_t i = 0; i < arg_vals.size() && i < f->arg_size(); ++i)
                    arg_vals[i] = coerceTo(arg_vals[i], f->getArg(i)->getType());
                llvm::CallInst *call = builder_->CreateCall(f, arg_vals);
                if (!call->getType()->isVoidTy())
                    call->setName("gate");
                return call;
            }

            throw std::runtime_error("MIR: unknown quantum gate '" + gate + "'");
        }
        if (kind == "NewObject")
        {
            const std::string className = rv.at("className").string();
            std::vector<llvm::Value *> arg_vals;
            for (const auto &a : rv.at("args").array())
                arg_vals.push_back(buildOperand(a));

            // 内建量子对象构造 → rvdl_create_* 内建（与 ir.ts 对齐）
            if (className == "BellState")
                return callObjectCtor("rvdl_create_BellState", {}, arg_vals);
            if (className == "DiracState")
                return callObjectCtor("rvdl_create_DiracState", {"i32"}, arg_vals);
            if (className == "QuantumRegister")
                return callObjectCtor("rvdl_create_QuantumRegister", {"i32"}, arg_vals);

            // lattice<T, B> 构造 → rvdl_lattice_new(rank, dim0, dim1, boundary)
            if (className.rfind("lattice<", 0) == 0)
            {
                std::vector<llvm::Value *> ctor;
                ctor.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), (int)arg_vals.size()));
                for (size_t i = 0; i < arg_vals.size() && i < 2; ++i)
                    ctor.push_back(coerceTo(arg_vals[i], llvm::Type::getInt32Ty(*ctx_)));
                while (ctor.size() < 3)
                    ctor.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 1));
                ctor.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0)); // boundary=open
                return callObjectCtor("rvdl_lattice_new", {"i32", "i32", "i32", "i32"}, ctor);
            }

            // form 类型等结构化构造：**未实现就报错，不返回空指针占位**。
            // 曾经的占位（ConstantPointerNull）会让后续代码对着 null 继续
            // 生成 IR —— 模块能 verifyModule 通过、能进 JIT，直到运行期
            // 段错误。把「没做完」变成一个看得见的错误（daemon 回
            // RESPONSE: ERROR - MIR compile failed: …），比一个静默的 null 好。
            throw std::runtime_error(
                "MIR: structured construction of '" + className +
                "' is not implemented yet (form/record ctor lowering); "
                "refusing to emit a null placeholder that would fail silently at runtime");
        }
        if (kind == "Member")
        {
            // form 字段访问：用 fieldIndex + fieldType 生成精确 getelementptr + load。
            llvm::Value *obj = buildOperand(rv.at("object"));
            if (rv.has("fieldIndex") && rv.has("fieldType"))
            {
                int fieldIndex = rv.at("fieldIndex").int_value();
                llvm::Type *fieldTy = mapType(rv.at("fieldType").string());
                // 从 object 的 local 类型查 form 结构类型，生成精确 getelementptr
                llvm::StructType *st = nullptr;
                const auto &objOp = rv.at("object");
                if (objOp.has("place") && objOp.at("place").has("local"))
                {
                    int local = objOp.at("place").at("local").int_value();
                    auto lt = local_types_.find(local);
                    if (lt != local_types_.end())
                    {
                        auto ft = form_types_.find(lt->second);
                        if (ft != form_types_.end()) st = ft->second;
                    }
                }
                if (st)
                {
                    llvm::Value *gep = builder_->CreateStructGEP(st, obj, fieldIndex, "fieldptr");
                    return builder_->CreateLoad(fieldTy, gep, "field");
                }
                // 无结构类型（object 非局部变量）：按 i8 偏移近似（首字段）
                llvm::Value *gep = builder_->CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*ctx_), obj, 8, "memptr");
                return builder_->CreateLoad(fieldTy, gep, "field");
            }
            // 无字段信息的 Member（如 QObject 属性）：同样不返回空指针占位 ——
            // 理由同上：静默的 null 会把「没做完」拖成运行期段错误。
            throw std::runtime_error(
                "MIR: member access without field info is not implemented yet; "
                "refusing to emit a null placeholder that would fail silently at runtime");
        }
        if (kind == "Ref")
        {
            // &x → 取 place 的 alloca 地址
            int local = rv.at("place").at("local").int_value();
            return local_alloca_.at(local);
        }
        if (kind == "Asm")
        {
            // 裸汇编块（多指令序列）：sideeffect InlineAsm，无输入/输出
            const std::string tpl = rv.at("template").string();
            llvm::FunctionType *ft = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), {}, false);
            llvm::InlineAsm *ia = llvm::InlineAsm::get(ft, tpl, "", true);
            return builder_->CreateCall(ia, {}, "asm");
        }
        if (kind == "Array")
        {
            // 数组字面量：alloca [N x T]，逐元素 store，再 load 出数组值（与局部数组值类型一致）
            const auto &elems = rv.at("elements").array();
            llvm::Type *elemTy = mapType(rv.at("elemTy").string());
            llvm::Type *arrTy = llvm::ArrayType::get(elemTy, elems.size());
            llvm::AllocaInst *alloc = builder_->CreateAlloca(arrTy, nullptr, "arrinit");
            for (size_t i = 0; i < elems.size(); ++i)
            {
                llvm::Value *ev = buildOperand(elems[i]);
                llvm::Value *gep = builder_->CreateConstInBoundsGEP2_32(arrTy, alloc, 0, (unsigned)i, "elemptr");
                builder_->CreateStore(coerceTo(ev, elemTy), gep);
            }
            return builder_->CreateLoad(arrTy, alloc, "arrval");
        }
        if (kind == "ArrayIndex")
        {
            // 数组索引：object 为 Ref（数组 alloca 指针），GEP [N x T] + load
            llvm::Value *obj = buildOperand(rv.at("object"));
            llvm::Value *idx = buildOperand(rv.at("index"));
            llvm::Type *elemTy = mapType(rv.at("elemTy").string());
            llvm::Type *arrTy = mapType(rv.at("arrTy").string()); // [N x T]
            llvm::Value *gep = builder_->CreateGEP(arrTy, obj,
                {builder_->getInt32(0), idx}, "elemptr");
            return builder_->CreateLoad(elemTy, gep, "elem");
        }
        throw std::runtime_error("MIR: unsupported rvalue kind '" + kind + "'");
    }

    /** 对象构造调用：declare + call，返回 opaque 指针 */
    llvm::Value *callObjectCtor(const char *symbol, const std::vector<const char *> &params,
                                const std::vector<llvm::Value *> &args)
    {
        std::vector<llvm::Type *> ptypes;
        for (const char *p : params) ptypes.push_back(mapLLVMType(p));
        std::vector<llvm::Value *> coerced = args;
        for (size_t i = 0; i < coerced.size() && i < ptypes.size(); ++i)
            coerced[i] = coerceTo(coerced[i], ptypes[i]);
        llvm::Function *f = declareFn(symbol, llvm::PointerType::get(*ctx_, 0), ptypes);
        return builder_->CreateCall(f, coerced, "obj");
    }

    // ---- 操作数 ---------------------------------------------------------------
    llvm::Value *buildOperand(const json::Value &op)
    {
        const std::string kind = op.at("kind").string();
        if (kind == "Const")
        {
            const json::Value &c = op.at("value");
            const std::string ck = c.at("kind").string();
            if (ck == "Int") return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), c.at("value").int64_value());
            if (ck == "Float") return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*ctx_), c.at("value").number());
            if (ck == "Bool") return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*ctx_), c.at("value").bool_value() ? 1 : 0);
            if (ck == "Str" || ck == "Char")
            {
                // 全局字符串常量
                const std::string s = c.at("value").string();
                return builder_->CreateGlobalString(s, "str");
            }
            throw std::runtime_error("MIR: unsupported const kind '" + ck + "'");
        }
        if (kind == "Copy" || kind == "Move")
        {
            int local = op.at("place").at("local").int_value();
            return builder_->CreateLoad(local_alloca_.at(local)->getAllocatedType(), local_alloca_.at(local), "loadtmp");
        }
        if (kind == "Ref")
        {
            int local = op.at("place").at("local").int_value();
            return local_alloca_.at(local);
        }
        throw std::runtime_error("MIR: unsupported operand kind '" + kind + "'");
    }

    // ---- 函数调用 -------------------------------------------------------------
    /** 调用并命名（void 返回不命名，避免 verifier "void value" 错误） */
    llvm::CallInst *emitCall(llvm::Function *f, const std::vector<llvm::Value *> &args)
    {
        llvm::CallInst *call = builder_->CreateCall(f, args);
        if (!call->getType()->isVoidTy())
            call->setName("calltmp");
        return call;
    }

    llvm::Value *buildCall(const json::Value &call)
    {
        const std::string target = call.at("target").string();
        const std::string ret_ty = call.has("retTy") && !call.at("retTy").is_null()
                                       ? call.at("retTy").string()
                                       : std::string("int32");

        // 特殊内建：原子 / 端口 I/O / 地址 / 锁 / 读寄存器 / volatile（生成 LLVM 特殊指令，非 declare+call）
        if (target == "sync_load" || target == "sync_store" || target == "sync_add" ||
            target == "sync_cas" || target == "sync_lock" || target == "sync_unlock" ||
            target == "outb" || target == "inb" || target == "outw" || target == "inw" ||
            target == "outl" || target == "inl" || target == "addr" ||
            target == "volatile_load" || target == "volatile_store" ||
            target == "read_cr0" || target == "read_cr2" || target == "read_cr3" || target == "read_cr4" ||
            target == "write_cr0" || target == "write_cr3" || target == "invlpg" ||
            target == "rdmsr" || target == "wrmsr" || target == "cpuid" ||
            target == "rdtsc" || target == "read_rflags" || target == "xgetbv")
            return buildSpecialCall(call, target);

        std::vector<llvm::Value *> arg_vals;
        for (const auto &a : call.at("args").array())
            arg_vals.push_back(buildOperand(a));

        // 用户函数：符号名 = target，签名由 MIR 的 retTy + 实参类型决定
        if (funcs_.count(target))
        {
            std::vector<llvm::Type *> arg_types;
            for (auto *v : arg_vals) arg_types.push_back(v->getType());
            llvm::Function *f = declareFn(target, mapType(ret_ty), arg_types);
            return emitCall(f, arg_vals);
        }

        // 内建函数：查签名表（精确 LLVM 符号名 + 签名 + 实参类型对齐）
        const auto &sigs = builtinSigs();
        auto it = sigs.find(target);
        if (it != sigs.end())
        {
            const BuiltinSig &sig = it->second;
            std::vector<llvm::Type *> param_types;
            for (const char *p : sig.params) param_types.push_back(mapLLVMType(p));

            for (size_t i = 0; i < arg_vals.size() && i < param_types.size(); ++i)
                arg_vals[i] = coerceTo(arg_vals[i], param_types[i]);

            llvm::Function *f = declareFn(sig.symbol, mapLLVMType(sig.ret), param_types);
            return emitCall(f, arg_vals);
        }

        // 未知函数：兜底 declare（符号名 = target）
        std::vector<llvm::Type *> arg_types;
        for (auto *v : arg_vals) arg_types.push_back(v->getType());
        llvm::Function *f = declareFn(target, mapType(ret_ty), arg_types);
        return emitCall(f, arg_vals);
    }

    llvm::Function *declareFn(const std::string &name, llvm::Type *ret, const std::vector<llvm::Type *> &args)
    {
        if (funcs_.count(name))
            return funcs_.at(name); // 用户函数已预声明
        llvm::FunctionType *ft = llvm::FunctionType::get(ret, args, false);
        llvm::Function *f = module_->getFunction(name);
        if (!f)
            f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, module_.get());
        return f;
    }

    // ---- 终结符 ---------------------------------------------------------------
    void buildTerminator(const json::Value &term)
    {
        const std::string kind = term.at("kind").string();
        if (kind == "Goto")
        {
            builder_->CreateBr(blocks_.at(term.at("target").int_value()));
        }
        else if (kind == "Return")
        {
            if (term.has("value") && !term.at("value").is_null())
                builder_->CreateRet(buildOperand(term.at("value")));
            else
                builder_->CreateRetVoid();
        }
        else if (kind == "Unreachable")
        {
            builder_->CreateUnreachable();
        }
        else if (kind == "SwitchInt")
        {
            llvm::Value *disc = buildOperand(term.at("discriminant"));
            // 收集 target；value==null 者为 default
            llvm::BasicBlock *default_bb = nullptr;
            std::vector<std::pair<int, llvm::BasicBlock *>> cases;
            for (const auto &t : term.at("targets").array())
            {
                llvm::BasicBlock *bb = blocks_.at(t.at("target").int_value());
                if (t.has("value") && !t.at("value").is_null())
                    cases.emplace_back(t.at("value").int_value(), bb);
                else
                    default_bb = bb;
            }
            if (!default_bb)
                default_bb = &current_fn_->back(); // 兜底
            if (cases.size() == 1)
            {
                // 单分支（无 default 的典型 if）→ icmp + 条件 br
                llvm::Value *cmp = builder_->CreateICmpEQ(disc, llvm::ConstantInt::get(disc->getType(), cases[0].first), "swcmp");
                builder_->CreateCondBr(cmp, cases[0].second, default_bb);
            }
            else
            {
                llvm::SwitchInst *sw = builder_->CreateSwitch(disc, default_bb, cases.size());
                for (const auto &[v, bb] : cases)
                    sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(disc->getType()), v), bb);
            }
        }
        else if (kind == "Call")
        {
            // Call 终结符（尾调用）：骨架阶段按 void 调用后 fallthrough 到 next
            buildCall(term);
            if (term.has("next"))
                builder_->CreateBr(blocks_.at(term.at("next").int_value()));
        }
        else
        {
            throw std::runtime_error("MIR: unsupported terminator kind '" + kind + "'");
        }
    }
};
}