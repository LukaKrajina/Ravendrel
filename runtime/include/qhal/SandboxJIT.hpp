#pragma once
#include "JIT.hpp"
#include "qml/QrcAbi.hpp"

#include <set>
#include <string>
#include <functional>
#include <stdexcept>
#include <iostream>
#include <memory>
#include <map>

#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"

namespace qhal
{
    inline std::map<std::string, void *> &native_symbols()
    {
        static std::map<std::string, void *> s;
        return s;
    }
    inline void register_native_symbol(const std::string &name, void *addr)
    {
        native_symbols()[name] = addr;
    }

    class RVDL_RT_API SandboxJIT
    {
    private:
        std::unique_ptr<llvm::orc::LLJIT> JIT_ptr;
        std::set<std::string> permissions_;

        static void handle_llvm_error(llvm::Error E)
        {
            llvm::handleAllErrors(std::move(E), [](llvm::ErrorInfoBase &EIB)
                                  { std::cerr << "Sandbox JIT Error: " << EIB.message() << std::endl; });
        }

    public:
        explicit SandboxJIT(const std::set<std::string> &permissions) : permissions_(permissions)
        {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();
            auto J = llvm::orc::LLJITBuilder()
                         .setLinkProcessSymbolsByDefault(false)
                         .setProcessSymbolsJITDylibSetup(
                             [](llvm::orc::LLJIT &LJ) -> llvm::Expected<llvm::orc::JITDylibSP>
                             {
                                auto &ES = LJ.getExecutionSession();
                                auto &JD = ES.createBareJITDylib("<Sandbox Process Symbols>");
                                auto AllowSymbol = [](const llvm::orc::SymbolStringPtr &Name) -> bool
                                {
                                    std::string n = (*Name).str();
                                    if (n.rfind("qk_", 0) == 0)
                                        return false;
                                    if (n.rfind("__quantum_", 0) == 0)
                                        return false;
                                    return true;
                                };
#if LLVM_VERSION_MAJOR >= 23
                                auto G = llvm::orc::EPCDynamicLibrarySearchGenerator::GetForTargetProcess(
                                    ES, LJ.getDylibMgr(), AllowSymbol);
#else
                                auto G = llvm::orc::EPCDynamicLibrarySearchGenerator::GetForTargetProcess(
                                    ES, AllowSymbol);
#endif
                                 if (!G)
                                     return G.takeError();
                                 JD.addGenerator(std::move(*G));
                                 return llvm::orc::JITDylibSP(&JD);
                             })
                         .create();
            if (!J)
            {
                handle_llvm_error(J.takeError());
                throw std::runtime_error("Sandbox LLJIT initialization failed");
            }
            JIT_ptr = std::move(*J);
            bind_whitelist();
        }

        void add_ir(const std::string &ir)
        {
            llvm::SMDiagnostic err;
            auto ctx = std::make_unique<llvm::LLVMContext>();
            auto mb = llvm::MemoryBuffer::getMemBuffer(ir);
            auto M = llvm::parseIR(*mb, err, *ctx);
            if (!M)
            {
                std::string err_str;
                llvm::raw_string_ostream os(err_str);
                err.print("SandboxJIT", os);
                throw std::runtime_error("Sandbox: invalid IR payload\n" + os.str());
            }

            auto E = JIT_ptr->addIRModule(llvm::orc::ThreadSafeModule(std::move(M), std::move(ctx)));
            if (E)
                handle_llvm_error(std::move(E));
        }

        template <typename Signature>
        std::function<Signature> get_function(const std::string &Name)
        {
            auto Sym = JIT_ptr->lookup(Name);
            if (!Sym)
                return nullptr;
            auto *FnPtr = llvm::jitTargetAddressToPointer<Signature *>(Sym->getValue());
            return [FnPtr](auto &&...args)
            {
                return FnPtr(std::forward<decltype(args)>(args)...);
            };
        }

        void *lookup_address(const std::string &Name)
        {
            auto Sym = JIT_ptr->lookup(Name);
            if (!Sym)
                return nullptr;
            return reinterpret_cast<void *>(Sym->getValue());
        }

        void bind_symbol(const std::string &Name, void *Addr)
        {
            auto &Dylib = JIT_ptr->getMainJITDylib();
            llvm::orc::MangleAndInterner M(JIT_ptr->getExecutionSession(), JIT_ptr->getDataLayout());
            llvm::orc::SymbolMap Map;
            Map[M(Name)] = llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(Addr),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            auto E = Dylib.define(llvm::orc::absoluteSymbols(Map));
            if (E)
                handle_llvm_error(std::move(E));
        }

    private:
        bool has(const std::string &p) const { return permissions_.count(p) != 0; }

        void bind_whitelist()
        {
            auto &Dylib = JIT_ptr->getMainJITDylib();
            llvm::orc::MangleAndInterner M(JIT_ptr->getExecutionSession(), JIT_ptr->getDataLayout());
            llvm::orc::SymbolMap Map;

            auto add = [&](const char *name, void *fn)
            {
                Map[M(name)] = llvm::orc::ExecutorSymbolDef(
                    llvm::orc::ExecutorAddr::fromPtr(fn),
                    llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
            };

            if (has("quantum.allocate"))
            {
                add("rvdl_alloc", (void *)&rvdl_alloc);
                add("rvdl_create_DiracState", (void *)&rvdl_create_DiracState);
                add("rvdl_create_BellState", (void *)&rvdl_create_BellState);
                add("rvdl_create_QuantumRegister", (void *)&rvdl_create_QuantumRegister);
                add("rvdl_create_basis_state", (void *)&rvdl_create_basis_state);
                add("__quantum__rt__qubit_allocate", (void *)&__quantum__rt__qubit_allocate);
            }
            if (has("quantum.measure"))
            {
                add("rvdl_measure", (void *)&rvdl_measure);
                add("rvdl_measure_object", (void *)&rvdl_measure_object);
                add("__quantum__qis__measure_int", (void *)&__quantum__qis__measure_int);
            }
            if (has("quantum.gate"))
            {
                add("__quantum__qis__h", (void *)&__quantum__qis__h);
                add("__quantum__qis__x", (void *)&__quantum__qis__x);
                add("__quantum__qis__y", (void *)&__quantum__qis__y);
                add("__quantum__qis__z", (void *)&__quantum__qis__z);
                add("__quantum__qis__s", (void *)&__quantum__qis__s);
                add("__quantum__qis__t", (void *)&__quantum__qis__t);
                add("__quantum__qis__rz", (void *)&__quantum__qis__rz);
                add("__quantum__qis__rx", (void *)&__quantum__qis__rx);
                add("__quantum__qis__ry", (void *)&__quantum__qis__ry);
                add("__quantum__qis__cnot", (void *)&__quantum__qis__cnot);
                add("__quantum__qis__toffoli", (void *)&__quantum__qis__toffoli);
                add("__quantum__qis__swap", (void *)&__quantum__qis__swap);
                add("__quantum__qis__qft", (void *)&__quantum__qis__qft);
                add("__quantum__qis__iqft", (void *)&__quantum__qis__iqft);
                add("__quantum__qis__cqft", (void *)&__quantum__qis__cqft);
                add("__quantum__qis__braid", (void *)&__quantum__qis__braid);
                add("__quantum__qis__cbraid", (void *)&__quantum__qis__cbraid);
                add("rvdl_spawn", (void *)&rvdl_spawn);
                add("__quantum__qis__cx", (void *)&__quantum__qis__cx);
                add("__quantum__qis__ch", (void *)&__quantum__qis__ch);
                add("__quantum__qis__crz", (void *)&__quantum__qis__crz);
                add("__quantum__qis__cswap", (void *)&__quantum__qis__cswap);
                add("__quantum__qis__c_toffoli", (void *)&__quantum__qis__c_toffoli);
                add("__quantum__qis__apply_noise", (void *)&__quantum__qis__apply_noise);
                add("__quantum__qis__measure_basis", (void *)&__quantum__qis__measure_basis);
            }
            if (has("quantum.release"))
            {
                add("rvdl_release_object", (void *)&rvdl_release_object);
                add("__quantum__rt__qubit_release", (void *)&__quantum__rt__qubit_release);
            }
            if (has("qlm.encode"))
            {
                add("rvdl_encode_text", (void *)&rvdl_encode_text);
                add("rvdl_encode_string", (void *)&rvdl_encode_string);
            }
            if (has("qlm.train"))
            {
                add("rvdl_qlm_invoke", (void *)&rvdl_qlm_invoke);
                add("rvdl_mind_train", (void *)&rvdl_mind_train);
                add("rvdl_veda_qlm_train", (void *)&rvdl_veda_qlm_train);
            }
            if (has("qlm.infer"))
            {
                add("rvdl_qlm_load", (void *)&rvdl_qlm_load);
                add("rvdl_qlm_forward", (void *)&rvdl_qlm_forward);
                add("rvdl_decode_string", (void *)&rvdl_decode_string);
            }

            if (has("sys.log"))
            {
                add("rvdl_sys_log", (void *)&rvdl_sys_log);
                add("rvdl_sys_logi", (void *)&rvdl_sys_logi);
            }
            if (has("sys.call"))
            {
                add("rvdl_sys_call", (void *)&rvdl_sys_call);
                add("rvdl_sys_calld", (void *)&rvdl_sys_calld);
                add("rvdl_sys_callp", (void *)&rvdl_sys_callp);
            }
            if (has("sys.mem"))
            {
                add("rvdl_gc_alloc", (void *)&rvdl_gc_alloc);
                add("rvdl_gc_free", (void *)&rvdl_gc_free);
            }
            
            if (has("math.qms"))
            {
                add("rvdl_qms_gap", (void *)&rvdl_qms_gap);
                add("rvdl_mix_bound", (void *)&rvdl_mix_bound);
                add("rvdl_qms_conc", (void *)&rvdl_qms_conc);
            }

            add("rvdl_qgate_h", (void *)&rvdl_qgate_h);
            add("rvdl_qgate_x", (void *)&rvdl_qgate_x);
            add("rvdl_qgate_rz", (void *)&rvdl_qgate_rz);
            add("rvdl_qgate_cnot", (void *)&rvdl_qgate_cnot);
            add("rvdl_qgate_cnot_pair", (void *)&rvdl_qgate_cnot_pair);
            add("rvdl_qexpect_z", (void *)&rvdl_qexpect_z);
            add("rvdl_qmeasure", (void *)&rvdl_qmeasure);
            add("rvdl_qobj_num_qubits", (void *)&rvdl_qobj_num_qubits);
            add("rvdl_qattention", (void *)&rvdl_qattention);
            add("rvdl_qstate_entropy", (void *)&rvdl_qstate_entropy);
            add("rvdl_qstate_fidelity", (void *)&rvdl_qstate_fidelity);
            add("rvdl_qstate_purity", (void *)&rvdl_qstate_purity);
            add("rvdl_qstate_linear_entropy", (void *)&rvdl_qstate_linear_entropy);
            add("rvdl_qstate_renyi_entropy", (void *)&rvdl_qstate_renyi_entropy);
            add("rvdl_math_dot", (void *)&rvdl_math_dot);
            add("rvdl_math_norm", (void *)&rvdl_math_norm);
            add("rvdl_geodesic_distance", (void *)&rvdl_geodesic_distance);
            add("rvdl_inversion", (void *)&rvdl_inversion);
            add("rvdl_hyperbolic_metric", (void *)&rvdl_hyperbolic_metric);
            add("rvdl_hyperbolic_distance", (void *)&rvdl_hyperbolic_distance);
            add("rvdl_dla_dim", (void *)&rvdl_dla_dim);
            add("rvdl_shannon4", (void *)&rvdl_shannon4);
            add("rvdl_shannon8", (void *)&rvdl_shannon8);
            add("rvdl_surrogate", (void *)&rvdl_surrogate);
            add("rvdl_tanh_quantize", (void *)&rvdl_tanh_quantize);
            add("rvdl_lif_step", (void *)&rvdl_lif_step);
            add("rvdl_mellowmax2", (void *)&rvdl_mellowmax2);
            add("rvdl_logsumexp2", (void *)&rvdl_logsumexp2);
            add("rvdl_boltzmann2", (void *)&rvdl_boltzmann2);
            add("rvdl_tnorm_luk", (void *)&rvdl_tnorm_luk);
            add("rvdl_tnorm_prod", (void *)&rvdl_tnorm_prod);
            add("rvdl_tnorm_godel", (void *)&rvdl_tnorm_godel);
            add("rvdl_polymer_weight", (void *)&rvdl_polymer_weight);
            add("rvdl_polymer_mix_bound", (void *)&rvdl_polymer_mix_bound);
            add("rvdl_tau_alpha", (void *)&rvdl_tau_alpha);
            add("rvdl_tau_generational", (void *)&rvdl_tau_generational);
            add("rvdl_tau_fold_depth", (void *)&rvdl_tau_fold_depth);
            add("rvdl_tau_gear_ratio", (void *)&rvdl_tau_gear_ratio);
            add("rvdl_tau_route_cost", (void *)&rvdl_tau_route_cost);
            add("rvdl_tau_route_price", (void *)&rvdl_tau_route_price);
            add("rvdl_qms_gap", (void *)&rvdl_qms_gap);
            add("rvdl_mix_bound", (void *)&rvdl_mix_bound);
            add("rvdl_qms_conc", (void *)&rvdl_qms_conc);
            add("rvdl_encode_amplitudes", (void *)&rvdl_encode_amplitudes);
            add("rvdl_encode_text", (void *)&rvdl_encode_text);
            add("rvdl_encode_adaptive", (void *)&rvdl_encode_adaptive);
            add("rvdl_encode_image", (void *)&rvdl_encode_image);
            add("rvdl_encode_pixel", (void *)&rvdl_encode_pixel);

            add("rvdl_retrocausal_imax", (void *)&rvdl_retrocausal_imax);
            add("rvdl_retrocausal_idoe", (void *)&rvdl_retrocausal_idoe);
            add("rvdl_retrocausal_q_capacity", (void *)&rvdl_retrocausal_q_capacity);
            add("rvdl_retrocausal_c_capacity", (void *)&rvdl_retrocausal_c_capacity);
            add("rvdl_retrocausal_q_one_shot", (void *)&rvdl_retrocausal_q_one_shot);
            add("rvdl_retrocausal_gain", (void *)&rvdl_retrocausal_gain);
            add("rvdl_retrocausal_deformed", (void *)&rvdl_retrocausal_deformed);
            add("rvdl_retrocausal_ctc_q_capacity", (void *)&rvdl_retrocausal_ctc_q_capacity);
            add("rvdl_retrocausal_ctc_c_capacity", (void *)&rvdl_retrocausal_ctc_c_capacity);
            add("rvdl_retrocausal_ctc_gain", (void *)&rvdl_retrocausal_ctc_gain);
            add("rvdl_retrocausal_ctc_dephasing", (void *)&rvdl_retrocausal_ctc_dephasing);

            add("rvdl_qrc_new", (void *)&rvdl_qrc_new);
            add("rvdl_qrc_train", (void *)&rvdl_qrc_train);
            add("rvdl_qrc_probe", (void *)&rvdl_qrc_probe);
            add("rvdl_qrc_predict", (void *)&rvdl_qrc_predict);
            add("rvdl_qrc_release", (void *)&rvdl_qrc_release);

            add("rvdl_lattice_new", (void *)&rvdl_lattice_new);
            add("rvdl_lattice_free", (void *)&rvdl_lattice_free);
            add("rvdl_lattice_ref", (void *)&rvdl_lattice_ref);
            add("rvdl_lattice_set", (void *)&rvdl_lattice_set);
            add("rvdl_lattice_rank", (void *)&rvdl_lattice_rank);
            add("rvdl_lattice_size", (void *)&rvdl_lattice_size);
            add("rvdl_lattice_boundary", (void *)&rvdl_lattice_boundary);

            add("rvdl_cgfx_rect", (void *)&rvdl_cgfx_rect);
            add("rvdl_cgfx_line", (void *)&rvdl_cgfx_line);
            add("rvdl_cgfx_polyline", (void *)&rvdl_cgfx_polyline);
            add("rvdl_cgfx_ellipse", (void *)&rvdl_cgfx_ellipse);
            add("rvdl_cgfx_triangle", (void *)&rvdl_cgfx_triangle);
            add("rvdl_cgfx_rect_a", (void *)&rvdl_cgfx_rect_a);
            add("rvdl_cgfx_line_a", (void *)&rvdl_cgfx_line_a);
            add("rvdl_cgfx_polyline_a", (void *)&rvdl_cgfx_polyline_a);
            add("rvdl_cgfx_ellipse_a", (void *)&rvdl_cgfx_ellipse_a);
            add("rvdl_cgfx_triangle_a", (void *)&rvdl_cgfx_triangle_a);
            add("rvdl_cgui_init", (void *)&rvdl_cgui_init);
            add("rvdl_cgui_should_close", (void *)&rvdl_cgui_should_close);
            add("rvdl_cgui_begin_frame", (void *)&rvdl_cgui_begin_frame);
            add("rvdl_cgui_end_frame", (void *)&rvdl_cgui_end_frame);
            add("rvdl_cgui_button", (void *)&rvdl_cgui_button);
            add("rvdl_cgui_text", (void *)&rvdl_cgui_text);
            add("rvdl_cgui_text_int", (void *)&rvdl_cgui_text_int);
            add("rvdl_cgui_beep", (void *)&rvdl_cgui_beep);
            add("rvdl_cgui_width", (void *)&rvdl_cgui_width);
            add("rvdl_cgui_height", (void *)&rvdl_cgui_height);
            add("rvdl_cgui_panel", (void *)&rvdl_cgui_panel);
            add("rvdl_cgui_panel_end", (void *)&rvdl_cgui_panel_end);
            add("rvdl_cgui_row", (void *)&rvdl_cgui_row);
            add("rvdl_cgui_mouse_x", (void *)&rvdl_cgui_mouse_x);
            add("rvdl_cgui_mouse_y", (void *)&rvdl_cgui_mouse_y);
            add("rvdl_cgui_mouse_left_clicked", (void *)&rvdl_cgui_mouse_left_clicked);

            for (const auto &[name, addr] : native_symbols())
            {
                add(name.c_str(), addr);
            }

            add("rvdl_runtime_run_topology", (void *)&rvdl_runtime_run_topology);

            add("___chkstk_ms", (void *)&rvdl_chkstk_stub);

            auto E = Dylib.define(llvm::orc::absoluteSymbols(Map));
            if (E)
                handle_llvm_error(std::move(E));
        }
    };
}