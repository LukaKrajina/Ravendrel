#pragma once

#include <string>
#include <iostream>
#include <system_error>
#include <vector>
#include <optional>
#include <sstream>
#include <cstdlib>

#include "Export.hpp"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Program.h"

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace rvdl
{

    struct AotToolchainOverrides
    {
        std::string clangxx;
        std::string sysroot;
        std::string rt_dir;
        std::string extra_libs;
    };

    namespace
    {
        void anchor() {}

        std::string locate_runtime_lib_dir()
        {
#if defined(__linux__) || defined(__APPLE__)
            Dl_info info{};
            if (dladdr(reinterpret_cast<void *>(&anchor), &info) && info.dli_fname)
            {
                std::string full(info.dli_fname);
                std::size_t pos = full.find_last_of("/\\");
                return (pos == std::string::npos) ? std::string{} : full.substr(0, pos);
            }
#endif
            return {};
        }
    }


    class RVDL_RT_API AOTCompiler
    {
    public:
        static const char *toolchain_env(const std::string &override_value, const char *env_name)
        {
            if (!override_value.empty())
                return override_value.c_str();
            return std::getenv(env_name);
        }

        static bool compile_to_binary(llvm::Module *module, const std::string &arch, const std::string &mode,
                                      const std::string &output_name,
                                      const AotToolchainOverrides &overrides = {})
        {
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmParsers();
            llvm::InitializeAllAsmPrinters();
            std::string target_triple;
#ifdef _WIN32
            if (arch == "x64")
                target_triple = "x86_64-pc-windows-msvc";
            else if (arch == "x32")
                target_triple = "i386-pc-windows-msvc";
            else if (arch == "arm64")
                target_triple = "aarch64-pc-windows-msvc";
            else if (arch == "android" || arch == "arm64-android")
                target_triple = "aarch64-linux-android";
#else
            if (arch == "x64")
                target_triple = "x86_64-pc-linux-gnu";
            else if (arch == "x32")
                target_triple = "i386-pc-linux-gnu";
            else if (arch == "arm64")
                target_triple = "aarch64-linux-gnu";
            else if (arch == "android" || arch == "arm64-android")
                target_triple = "aarch64-linux-android";
#endif
            if (target_triple.empty())
            {
                target_triple = llvm::sys::getDefaultTargetTriple();
            }
            llvm::Triple triple(target_triple);

            std::string error;
#if LLVM_VERSION_MAJOR >= 22
            auto target = llvm::TargetRegistry::lookupTarget(triple, error);
#else
            auto target = llvm::TargetRegistry::lookupTarget(triple.str(), error);
#endif
            if (!target)
            {
                llvm::errs() << "[AOT Error] " << error << "\n";
                return false;
            }

            llvm::TargetOptions opt;
            std::optional<llvm::Reloc::Model> reloc_model = std::nullopt;
            if (mode == "-m")
            {
                reloc_model = llvm::Reloc::PIC_;
            }

#if LLVM_VERSION_MAJOR >= 22
            auto target_machine = target->createTargetMachine(triple, "generic", "", opt, reloc_model);
#else
            auto target_machine = target->createTargetMachine(triple.str(), "generic", "", opt, reloc_model);
#endif
            module->setDataLayout(target_machine->createDataLayout());
            
#if LLVM_VERSION_MAJOR >= 21
            module->setTargetTriple(triple);
#else
            module->setTargetTriple(triple.str());
#endif
            std::string obj_filename = output_name + ".o";
            std::error_code ec;
            llvm::raw_fd_ostream dest(obj_filename, ec, llvm::sys::fs::OF_None);

            if (ec)
            {
                llvm::errs() << "[AOT Error] Could not open file: " << ec.message() << "\n";
                return false;
            }

            llvm::legacy::PassManager pass;
            if (target_machine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
            {
                llvm::errs() << "[AOT Error] TargetMachine can't emit a file of this type.\n";
                return false;
            }

            pass.run(*module);
            dest.flush();
            dest.close();
            const bool is_android = (arch == "android" || arch == "arm64-android");
            std::string ext = "";
            if (mode == "-m")
            {
#ifdef _WIN32
                ext = is_android ? ".so" : ".dll";
#elif __APPLE__
                ext = is_android ? ".so" : ".dylib";
#else
                ext = ".so";
#endif
            }
            else
            {
#ifdef _WIN32
                ext = is_android ? "" : ".exe";
#endif
            }

            std::string final_output = output_name + ext;
            std::cout << "[Ravendrel AOT] LLVM Object file '" << obj_filename << "' generated.\n";
            std::cout << "[Ravendrel AOT] Spawning system linker for -> " << final_output << "\n";
            std::vector<llvm::StringRef> candidate_linkers;
            if (is_android)
            {
                candidate_linkers = {
                    "aarch64-linux-android21-clang++",
                    "aarch64-linux-android24-clang++",
                    "aarch64-linux-android26-clang++",
                };
            }
            else
            {
#ifdef _WIN32
                candidate_linkers = { "clang++.exe", "clang.exe", "g++.exe", "gcc.exe" };
#else
                candidate_linkers = { "clang++", "clang", "g++", "gcc" };
#endif
            }

            llvm::ErrorOr<std::string> linker_path = std::errc::no_such_file_or_directory;
            if (is_android)
            {
                if (const char *explicit_linker = toolchain_env(overrides.clangxx, "RVDL_ANDROID_CLANGXX"))
                {
                    linker_path = std::string(explicit_linker);
                }
            }
            if (!linker_path)
            {
                for (const auto &candidate : candidate_linkers)
                {
                    linker_path = llvm::sys::findProgramByName(candidate);
                    if (linker_path)
                        break;
                }
            }

            if (!linker_path)
            {
                if (is_android)
                {
                    llvm::errs() << "[AOT Error] Android 目标需要 NDK 交叉编译器：未在 PATH 中找到 "
                                    "aarch64-linux-android21/24/26-clang++。\n"
                                 << "            请安装 Android NDK 并加入 PATH，或用环境变量 "
                                    "RVDL_ANDROID_CLANGXX 指定其绝对路径，\n"
                                    << "            必要时同时设置 RVDL_ANDROID_SYSROOT（NDK sysroot）与 "
                                    "RVDL_ANDROID_RT_DIR（交叉编译出的 librvdl_rt.so 目录）。\n"
                                 << "            打包 APK 请用命令行：rvdl build apk <file.rvdl> [--release]\n";
                }
                else
                {
                    llvm::errs() << "[AOT Error] Linker failed: No suitable C++ linker (clang/gcc) found in system PATH.\n";
                }
                return false;
            }

            std::vector<llvm::StringRef> linker_args;
            linker_args.push_back(linker_path.get());

            if (mode == "-m")
            {
                linker_args.push_back("-shared");
            }

            if (arch == "x32")
                linker_args.push_back("-m32");

            std::string android_sysroot_arg;
            if (is_android)
            {
                if (const char *sysroot = toolchain_env(overrides.sysroot, "RVDL_ANDROID_SYSROOT"))
                {
                    android_sysroot_arg = "--sysroot=" + std::string(sysroot);
                    linker_args.push_back(android_sysroot_arg);
                }
            }

            linker_args.push_back(obj_filename);
            linker_args.push_back("-o");
            linker_args.push_back(final_output);
            std::string lib_dir = locate_runtime_lib_dir();
            if (is_android)
            {
                if (const char *rt_dir = toolchain_env(overrides.rt_dir, "RVDL_ANDROID_RT_DIR"))
                    lib_dir = rt_dir;
            }
            std::string lib_dir_link_arg;
            std::string rpath_arg;

            if (!lib_dir.empty())
                lib_dir_link_arg = "-L" + lib_dir;

#if defined(__APPLE__)
            rpath_arg = "-Wl,-rpath,@loader_path";
#elif !defined(_WIN32)
            rpath_arg = "-Wl,-rpath,$ORIGIN";
#endif

            if (!lib_dir.empty() && !rpath_arg.empty())
                rpath_arg += ":" + lib_dir;

            if (!lib_dir_link_arg.empty())
                linker_args.push_back(lib_dir_link_arg);
            linker_args.push_back("-L.");
            linker_args.push_back("-lrvdl_rt");
            if (const char *extra = toolchain_env(overrides.extra_libs, "RVDL_AOT_EXTRA_LIBS"))
            {
                std::istringstream iss(extra);
                std::string tok;
                while (iss >> tok)
                    linker_args.push_back(tok);
            }
            if (!rpath_arg.empty())
                linker_args.push_back(rpath_arg);

            std::string err_msg;

            int link_res = llvm::sys::ExecuteAndWait(
                linker_path.get(),
                linker_args,
                std::nullopt,
                {},
                0,
                0,
                &err_msg);

            if (link_res != 0)
            {
                llvm::errs() << "[AOT Error] Linker failed (Exit " << link_res << "): " << err_msg << "\n";
                return false;
            }

            llvm::sys::fs::remove(obj_filename);
            std::cout << "[Ravendrel AOT] Successfully compiled and linked: " << final_output << "\n";
            return true;
        }
    };
}