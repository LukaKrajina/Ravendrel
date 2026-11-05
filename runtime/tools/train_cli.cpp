// ============================================================================
// train_cli.cpp —— Jarvis 命令行训练器
//
// 复用 Transmitter.cpp 的训练链路（QVM 后端 + QLM + qqnt tokenizer + LF2C2
// 上下文编码 + QDataState 样本），去掉 GUI 层，变成可脚本化的 CLI：
//
//   train_cli <train.txt> <output.rvdlm> [epochs] [lr]
//
// 数据格式：纯文本（清洗后的 manifest/*/train.txt）。逐 token（字节）构造
// 下一个 token 预测样本：16 qubit 上下文态（LF2C2 压缩最近 32 token）→
// 目标 token。训练后导出 .rvdlm，供 rvdl serve 推理。
//
// 编译（链接预编译的 librvdl_rt.so，不重编核心）：
//   g++ -std=c++20 -O2 -I runtime/include -I runtime/src \
//       runtime/tools/train_cli.cpp -o train_cli \
//       -L ~/.local/lib/ravendrel/<ver>/bin -lrvdl_rt \
//       -Wl,-rpath,$HOME/.local/lib/ravendrel/<ver>/bin
// ============================================================================

#include "qhal/QVM.hpp"
#include "qhal/GpuBackend.hpp"
#include "qlm/QLM.hpp"
#include "qml/Inference.hpp"
#include <Kokkos_Core.hpp>
#include <omp.h>

#include <cstdlib>
#include <cstring>
#include <chrono>
#ifdef _WIN32
// Windows 没有 dlfcn：GPU 探测改用 LoadLibraryA/GetProcAddress（cudart/cuda）。
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// 复刻 runtime_api.cpp 的 detect_gpu_available：加载 libcudart/libcuda +
// cuInit + cuDeviceGetCount，探测成功后调用 rvdl::set_gpu_backend_available(true)，
// 让 Ga.hpp 走 Kokkos CUDA 后端而非 CPU OpenMP。训练器不走 rvdl_runtime_create()
// （那是 daemon 入口，会拉起完整 JIT），故在此自包含地初始化 GPU。
static bool detect_and_enable_gpu()
{
    const char *nogpu = std::getenv("RVDL_NO_GPU");
    if (nogpu && nogpu[0] && std::strcmp(nogpu, "0") != 0 &&
        std::strcmp(nogpu, "false") != 0 && std::strcmp(nogpu, "no") != 0 &&
        std::strcmp(nogpu, "off") != 0)
        return false;
#ifdef _WIN32
    for (const char *n : {"cudart64_13.dll", "cudart64_12.dll", "cudart.dll", "nvcuda.dll"})
        LoadLibraryA(n);
    HMODULE cuda = LoadLibraryA("nvcuda.dll");
    if (!cuda)
        return false;
    auto cuInit = reinterpret_cast<int (*)(unsigned int)>(GetProcAddress(cuda, "cuInit"));
    auto cuGetCount = reinterpret_cast<int (*)(int *)>(GetProcAddress(cuda, "cuDeviceGetCount"));
    if (!cuInit || !cuGetCount)
        return false;
    if (cuInit(0) != 0)
        return false;
    int count = 0;
    return (cuGetCount(&count) == 0) && count > 0;
#else
    for (const char *n : {"libcudart.so", "libcudart.so.13", "libcudart.so.12", "libcudart.so.11.0"})
        dlopen(n, RTLD_NOW | RTLD_GLOBAL);
    void *cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!cuda)
        return false;
    auto cuInit = reinterpret_cast<int (*)(unsigned int)>(dlsym(cuda, "cuInit"));
    auto cuGetCount = reinterpret_cast<int (*)(int *)>(dlsym(cuda, "cuDeviceGetCount"));
    if (!cuInit || !cuGetCount)
        return false;
    if (cuInit(0) != 0)
        return false;
    int count = 0;
    return (cuGetCount(&count) == 0) && count > 0;
#endif
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: train_cli <train.txt> <output.rvdlm> [epochs] [lr] [layers] [--continue <old.rvdlm>]\n";
        return 1;
    }
    // 参数解析：位置参数（向后兼容）+ 可选 --continue / --epochs / --lr / --layers
    std::string input_file, output_file, cont_path;
    int epochs = 1;
    double lr = 0.02;
    int layers = 2;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--continue" && i + 1 < argc) { cont_path = argv[++i]; }
        else if (a == "--epochs" && i + 1 < argc) { epochs = std::atoi(argv[++i]); }
        else if (a == "--lr" && i + 1 < argc) { lr = std::atof(argv[++i]); }
        else if (a == "--layers" && i + 1 < argc) { layers = std::atoi(argv[++i]); }
        else positional.push_back(a);
    }
    if (positional.size() < 2)
    {
        std::cerr << "Usage: train_cli <train.txt> <output.rvdlm> [epochs] [lr] [layers] [--continue <old.rvdlm>]\n";
        return 1;
    }
    input_file = positional[0];
    output_file = positional[1];
    if (positional.size() > 2) epochs = std::atoi(positional[2].c_str());
    if (positional.size() > 3) lr = std::atof(positional[3].c_str());
    if (positional.size() > 4) layers = std::atoi(positional[4].c_str());

    std::cout << "[Train] input=" << input_file << " output=" << output_file
              << " epochs=" << epochs << " lr=" << lr << "\n";

    // 训练是 host 单线程门操作（16-qubit 稠密态），OpenMP 线程池只会空转：
    // 实测 user 时间被空转到 real 的 ~15 倍，但 real 不降。用 omp_set_num_threads
    // 显式压到 1（setenv 在 main 里对 libgomp 无效——它在进程启动时已缓存线程数）。
    // 用户可显式 OMP_NUM_THREADS=N 覆盖（此处尊重之）。
    if (!std::getenv("OMP_NUM_THREADS"))
        omp_set_num_threads(1);

    // GPU 探测：dlopen libcuda + cuDeviceGetCount 确认可用，写入
    // gpu_backend_available，让 Ga.hpp 走 Kokkos CUDA 后端（而非 CPU OpenMP）。
    // 必须在 Kokkos::initialize 之前。
    const bool gpu = detect_and_enable_gpu();
    rvdl::set_gpu_backend_available(gpu);
    if (gpu)
        std::cout << "[Train] CUDA GPU detected. Using Kokkos CUDA backend.\n";
    else
        std::cout << "[Train] No CUDA GPU. Falling back to CPU (OpenMP) backend.\n";

    // Kokkos 必须先初始化：QVM 的 GPU 后端（Active Policy 2）用 Kokkos View，
    // 未 initialize 会 abort（"View constructed before initialize"）。
    Kokkos::initialize(argc, argv);

    qhal::QVM vm;
    // QDataState 用固定 id {0..15}（与 Transmitter/Inference 一致），故需先把
    // 这 16 个物理 qubit 分配出来（QVM 默认构造 num_qubits=0，否则 apply_rz(0)
    // 报 "qubit id out of range: 0 >= 0"）。
    vm.allocate_qubits(16);
    global_qm = &vm;
    qlm::QLM lm(&vm, /*qubits=*/16, /*layers=*/layers);

    // 读训练文本
    std::ifstream fin(input_file);
    if (!fin.is_open())
    {
        std::cerr << "[Train] cannot open " << input_file << "\n";
        return 1;
    }
    std::stringstream buf;
    buf << fin.rdbuf();
    const std::string text = buf.str();
    const std::vector<uint8_t> tokens = qqnt::global_tokenizer.encode(text);
    std::cout << "[Train] tokens=" << tokens.size() << "\n";
    if (tokens.size() < 3)
    {
        std::cerr << "[Train] too few tokens (< 3), nothing to learn.\n";
        return 1;
    }

    // 构造样本：对每个位置 i（从 2 起），用前 i 个 token 构造 16 qubit 上下文态，
    // 目标是 tokens[i]（下一个 token 预测，与 Transmitter / Inference 一致）。
    const std::vector<size_t> qubit_ids = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::vector<std::shared_ptr<rvdl::QObject>> dataset;
    dataset.reserve(tokens.size() - 2);
    for (size_t i = 2; i < tokens.size(); ++i)
    {
        auto t0 = std::chrono::steady_clock::now();
        auto sample = std::make_shared<rvdl::QDataState>(&vm, qubit_ids);
        const std::vector<uint8_t> history(tokens.begin(),
                                           tokens.begin() + static_cast<long>(i));
        qlm::lf2c2::encode_context(&vm, qlm::lf2c2::ContextConfig{}, history,
                                   sample->get_ids());
        sample->qlm_data = new uint8_t(tokens[i]);
        dataset.push_back(sample);
        auto t1 = std::chrono::steady_clock::now();
        std::cout << "[Train] sample " << i << "/" << tokens.size() << " took "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                  << " ms\n" << std::flush;
    }
    std::cout << "[Train] samples=" << dataset.size() << "\n";

    // 增量训练（「烧录」）：若指定 --continue，加载现有 .rvdlm 权重作为初始值，
    // 在现有能力上继续训练，而非从零重训。
    std::vector<double> init_weights;
    const std::vector<double> *init_ptr = nullptr;
    if (!cont_path.empty())
    {
        try
        {
            auto loaded = qml::ModelExporter<double>::load(cont_path);
            init_weights.assign(loaded.tensor.data(),
                                loaded.tensor.data() + loaded.tensor.size());
            init_ptr = &init_weights;
            std::cout << "[Train] continue from " << cont_path
                      << " (" << init_weights.size() << " params)\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << "[Train] 无法加载 --continue 模型 '" << cont_path
                      << "': " << e.what() << "（从零初始化）\n";
        }
    }

    // 训练 + 导出 .rvdlm
    auto model = lm.train_and_export(epochs, lr, output_file, dataset, init_ptr);

    // 清理样本的 qlm_data（train_and_export 后所有权仍在我们这）
    for (auto &s : dataset)
    {
        delete static_cast<uint8_t *>(s->qlm_data);
        s->qlm_data = nullptr;
    }

    std::cout << "[Train] done: " << output_file
              << " params=" << model.tensor.size() << "\n";
    Kokkos::finalize();
    return 0;
}