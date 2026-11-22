#include "qhal/QVM.hpp"
#include "qhal/GpuBackend.hpp"
#include "qhal/QuantumAccel.hpp"
#include "qlm/QLM.hpp"
#include "qlm/AdjointDevice.hpp"
#include "qlm/BatchAdjoint.hpp"
#include "qlm/DeviceResources.hpp"
#include "qlm/TrainingData.hpp"
#include "qml/Inference.hpp"
#include "utils/A2L.hpp"
#include "qhal/SparseAmplitude.hpp"
#include "qhal/SparseResources.hpp"
#include "qhal/LowRankState.hpp"
#include "qlm/RasAdjoint.hpp"
#include "qlm/RasAdjointDevice.hpp"
#include "qlm/SparseAdjoint.hpp"
#include "utils/A2LCollective.hpp"
#include <Kokkos_Core.hpp>
#include <omp.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../../vendor/stb/stb_image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace
{
    constexpr size_t kReadoutBits = 8;

    inline size_t params_per_layer(size_t num_qubits)
    {
        return num_qubits + kReadoutBits;
    }

    std::atomic<int> g_sigint_count{0};

#ifdef _WIN32
    BOOL WINAPI console_ctrl_handler(DWORD type)
    {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
            return FALSE;

        const int count = g_sigint_count.fetch_add(1) + 1;
        if (count == 1)
        {
            std::fputs("\n[Train] 收到 Ctrl+C：将在当前样本结束后停止并保存模型；"
                       "再按一次 Ctrl+C 强制退出。\n",
                       stderr);
            return TRUE;
        }

        std::fputs("\n[Train] 第二次 Ctrl+C：强制退出（当前模型可能未保存）。\n",
                   stderr);
        std::fflush(stderr);
        ExitProcess(130);
    }

    void disable_console_quick_edit()
    {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(in, &mode))
        {
            mode &= ~ENABLE_QUICK_EDIT_MODE;
            mode |= ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(in, mode);
        }
    }

    void lower_process_priority()
    {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
#else
    void posix_signal_handler(int)
    {
        const int count = g_sigint_count.fetch_add(1) + 1;
        if (count >= 2)
        {
            const char msg[] = "\n[Train] 第二次 Ctrl+C：强制退出。\n";
            (void)!::write(STDERR_FILENO, msg, sizeof(msg) - 1);
            std::_Exit(130);
        }
        const char msg[] =
            "\n[Train] 收到 Ctrl+C：将在当前样本结束后停止并保存模型；"
            "再按一次 Ctrl+C 强制退出。\n";
        (void)!::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }

    void disable_console_quick_edit() {}
    void lower_process_priority() {}
#endif

    void install_signal_handlers(bool enable)
    {
        if (!enable)
            return;
#ifdef _WIN32
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
        std::signal(SIGINT, posix_signal_handler);
        std::signal(SIGTERM, posix_signal_handler);
#endif
    }

    [[maybe_unused]] bool detect_gpu()
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

    bool load_image_angles(const std::string &path, std::array<double, 8> &angles)
    {
        int w = 0, h = 0, channels = 0;
        unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 1);
        if (!pixels || w <= 0 || h <= 0)
            return false;

        constexpr int kCols = 4;
        constexpr int kRows = 2;
        for (int r = 0; r < kRows; ++r)
        {
            const int y0 = r * h / kRows;
            int y1 = (r + 1) * h / kRows;
            if (y1 <= y0)
                y1 = y0 + 1;
            for (int c = 0; c < kCols; ++c)
            {
                const int x0 = c * w / kCols;
                int x1 = (c + 1) * w / kCols;
                if (x1 <= x0)
                    x1 = x0 + 1;

                double acc = 0.0;
                size_t n = 0;
                for (int y = y0; y < y1 && y < h; ++y)
                    for (int x = x0; x < x1 && x < w; ++x)
                    {
                        acc += static_cast<double>(pixels[y * w + x]);
                        ++n;
                    }
                angles[r * kCols + c] =
                    M_PI * (n ? (acc / static_cast<double>(n)) / 255.0 : 0.0);
            }
        }
        stbi_image_free(pixels);
        return true;
    }

    std::string format_duration(double seconds)
    {
        if (!(seconds >= 0.0) || !std::isfinite(seconds))
            return "--:--";
        long long s = static_cast<long long>(seconds + 0.5);
        char buf[32] = {0};
        if (s >= 3600)
            std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld",
                          s / 3600, (s / 60) % 60, s % 60);
        else
            std::snprintf(buf, sizeof(buf), "%02lld:%02lld",
                          (s / 60) % 60, s % 60);
        return buf;
    }

    enum class DeviceMode
    {
        Auto,
        Cpu,
        Gpu
    };

    enum class GradientMode
    {
        Auto,
        Adjoint,
        ParamShift
    };

    struct CliOptions
    {
        std::string input_file;
        std::string output_file;
        std::string continue_path;
        std::string checkpoint_path;
        std::string image_root;

        int epochs = 1;
        double lr = 0.02;
        int layers = 2;
        bool layers_explicit = false;
        int qubits = 16;
        bool qubits_auto = false;
        bool qubits_explicit = false;
        bool image_mode = false;
        size_t limit = 0;

        size_t checkpoint_every = 0;
        GradientMode gradient = GradientMode::Auto;
        qlm::Optimizer optimizer = qlm::Optimizer::Adam;
        bool shuffle = false;
        uint64_t seed = 42;
        int patience = 0;
        double min_delta = 1e-5;
        double grad_clip = 10.0;
        double weight_decay = 0.0;
        DeviceMode device = DeviceMode::Auto;
        bool low_priority = false;
        bool skip_bad_images = false;
        bool quiet = false;
        bool show_help = false;

        size_t max_samples = 2000000;
        size_t moe_experts = 1;
        size_t moe_active_offset = 0;
        size_t batch_size = 1;
        size_t tile_bits = 0;
        double tile_epsilon = 0.0;
        size_t a2l_threads = 0;
        double sparse_epsilon = 0.0;
        size_t sparse_tile_bits = 0;
        double lowrank_epsilon = 0.0;
        size_t lowrank_rank = 0;
        std::string state_backend = "auto";
        bool dry_run = false;
        std::string json_log;
        std::string run_id;
        std::string data_fingerprint;
        uint64_t data_bytes = 0;
    };

    const char *kUsage =
        "Usage:\n"
        "  train_cli <train.txt> <output.rvdlm> [epochs] [lr] [layers] [options]\n"
        "  train_cli <captions.jsonl> <output.rvdlm> [epochs] [lr] [layers] --images [options]\n"
        "\n"
        "Input / output:\n"
        "  --continue <old.rvdlm>     从已有模型继续训练（加载失败直接报错）\n"
        "  --checkpoint <path>        周期性保存中间权重（.rvdlm）\n"
        "  --checkpoint-every <N>     每 N 个样本保存一次；缺省 / 0 = 每个 epoch 末\n"
        "  --images                   图文联合模式（输入是 caption JSON Lines）\n"
        "  --image-root <dir>         图像根目录（默认 = 清单所在目录）\n"
        "  --limit <N>                文本模式只读前 N 字节；图文模式只读前 N 行\n"
        "  --max-samples <N>          样本数硬上限（默认 2000000，防止 shuffle 内存爆炸）\n"
        "  --moe-experts <N>          MoE 稀疏专家组数（默认 1=稠密；Ultra 5pB 建议 32）\n"
        "  --moe-active-expert <N>    本进程的专家轮转起点（烧录分片按 shardIndex % N 传入）\n"
        "  --batch-size <N>           BAT minibatch：B 个样本共享一次 optimizer step（默认 1）\n"
        "  --tile-bits <N>            态振幅分块：按低 N 位分 2^N 个 tile 做局部梯度归约（默认 0）\n"
        "  --tile-epsilon <F>         EBAT：跳过 L2 质量 < eps^2 的 tile 并报告误差上界（默认 0=精确）\n"
        "  --a2l-threads <N>         A2L 高并发预检线程数（默认硬件并发）\n"
        "  --sparse-epsilon <F>       DQ-Sparse 分散量化预算（默认 0=Dense，保证 parity）\n"
        "  --sparse-tile-bits <N>     DQ-Sparse tile 位数（默认 6）\n"
        "  --lowrank-epsilon <F>      RAS 低秩态能量预算（0 = 关闭，仅旁路评估）\n"
        "  --lowrank-rank <N>         RAS 秩上限（默认 0 = 关闭）\n"
        "  --skip-bad-images          跳过无法解码的图像（默认直接报错）\n"
        "  --dry-run                  只做数据预检与配置校验，不训练、不写模型\n"
        "  --json-log <file.jsonl>    结构化事件日志（config/progress/done/error）\n"
        "  --run-id <id>              复现用运行标识（缺省自动生成）\n"
        "\n"
        "Optimization:\n"
        "  --epochs <N>               训练轮数（位置参数优先）\n"
        "  --lr <x>                   学习率（位置参数优先）\n"
        "  --layers <N>               QLM 层数（位置参数优先；--continue 时自动推断）\n"
        "  --qubits <N|auto>          QLM qubit 数（默认 16；auto = 设备最大，范围 8..20）\n"
        "  --optimizer adam|sgd       优化器（默认 adam）\n"
        "  --gradient auto|adjoint|param-shift\n"
        "                             梯度算法（auto：稠密态用 adjoint，否则回退）\n"
        "  --shuffle                  每个 epoch 打乱样本顺序\n"
        "  --seed <N>                 初始化 / shuffle 随机种子（默认 42）\n"
        "  --patience <N>             early stopping 容忍轮数（0 = 关闭）\n"
        "  --min-delta <x>            early stopping 最小改善量（默认 1e-5）\n"
        "  --grad-clip <x>            逐参数梯度裁剪（0 = 关闭，默认 10）\n"
        "  --weight-decay <x>         L2 权重衰减（默认 0）\n"
        "\n"
        "System:\n"
        "  --device auto|cpu|gpu      计算设备（默认 auto）\n"
        "  --low-priority             降低进程 / 线程优先级，避免抢占交互应用\n"
        "  --quiet                    不打印逐样本进度（仍打印 epoch 汇总）\n"
        "  -h, --help                 显示本帮助\n";

    bool parse_size(const std::string &text, size_t &out)
    {
        try
        {
            size_t pos = 0;
            unsigned long long v = std::stoull(text, &pos);
            if (pos != text.size())
                return false;
            out = static_cast<size_t>(v);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool parse_int(const std::string &text, int &out)
    {
        try
        {
            size_t pos = 0;
            long long v = std::stoll(text, &pos);
            if (pos != text.size() || v < std::numeric_limits<int>::min() ||
                v > std::numeric_limits<int>::max())
                return false;
            out = static_cast<int>(v);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool parse_double(const std::string &text, double &out)
    {
        try
        {
            size_t pos = 0;
            double v = std::stod(text, &pos);
            if (pos != text.size() || !std::isfinite(v))
                return false;
            out = v;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool parse_args(int argc, char **argv, CliOptions &opt)
    {
        std::vector<std::string> positional;

        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto need_value = [&](const char *name) -> const char *
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "[Train] 选项 " << name << " 缺少参数值\n";
                    return nullptr;
                }
                return argv[++i];
            };

            if (a == "-h" || a == "--help")
            {
                opt.show_help = true;
                return true;
            }
            else if (a == "--continue")
            {
                const char *v = need_value("--continue");
                if (!v) return false;
                opt.continue_path = v;
            }
            else if (a == "--checkpoint")
            {
                const char *v = need_value("--checkpoint");
                if (!v) return false;
                opt.checkpoint_path = v;
            }
            else if (a == "--checkpoint-every")
            {
                const char *v = need_value("--checkpoint-every");
                if (!v || !parse_size(v, opt.checkpoint_every))
                {
                    std::cerr << "[Train] --checkpoint-every 需要非负整数\n";
                    return false;
                }
            }
            else if (a == "--epochs")
            {
                const char *v = need_value("--epochs");
                if (!v || !parse_int(v, opt.epochs))
                {
                    std::cerr << "[Train] --epochs 需要整数\n";
                    return false;
                }
            }
            else if (a == "--lr")
            {
                const char *v = need_value("--lr");
                if (!v || !parse_double(v, opt.lr))
                {
                    std::cerr << "[Train] --lr 需要数值\n";
                    return false;
                }
            }
            else if (a == "--layers")
            {
                const char *v = need_value("--layers");
                if (!v || !parse_int(v, opt.layers))
                {
                    std::cerr << "[Train] --layers 需要整数\n";
                    return false;
                }
                opt.layers_explicit = true;
            }
            else if (a == "--qubits")
            {
                const char *v = need_value("--qubits");
                if (!v)
                    return false;
                const std::string qv = v;
                if (qv == "auto")
                {
                    opt.qubits_auto = true;
                    opt.qubits_explicit = true;
                }
                else if (parse_int(qv, opt.qubits))
                {
                    opt.qubits_explicit = true;
                }
                else
                {
                    std::cerr << "[Train] --qubits 需要整数或 auto\n";
                    return false;
                }
            }
            else if (a == "--images")
            {
                opt.image_mode = true;
            }
            else if (a == "--image-root")
            {
                const char *v = need_value("--image-root");
                if (!v) return false;
                opt.image_root = v;
            }
            else if (a == "--limit")
            {
                const char *v = need_value("--limit");
                if (!v || !parse_size(v, opt.limit))
                {
                    std::cerr << "[Train] --limit 需要非负整数\n";
                    return false;
                }
            }
            else if (a == "--optimizer")
            {
                const char *v = need_value("--optimizer");
                if (!v) return false;
                const std::string name = v;
                if (name == "adam")
                    opt.optimizer = qlm::Optimizer::Adam;
                else if (name == "sgd")
                    opt.optimizer = qlm::Optimizer::SGD;
                else
                {
                    std::cerr << "[Train] --optimizer 只支持 adam / sgd\n";
                    return false;
                }
            }
            else if (a == "--gradient")
            {
                const char *v = need_value("--gradient");
                if (!v) return false;
                const std::string name = v;
                if (name == "auto")
                    opt.gradient = GradientMode::Auto;
                else if (name == "adjoint")
                    opt.gradient = GradientMode::Adjoint;
                else if (name == "param-shift" || name == "param_shift")
                    opt.gradient = GradientMode::ParamShift;
                else
                {
                    std::cerr << "[Train] --gradient 只支持 auto / adjoint / param-shift\n";
                    return false;
                }
            }
            else if (a == "--shuffle")
            {
                opt.shuffle = true;
            }
            else if (a == "--seed")
            {
                const char *v = need_value("--seed");
                size_t seed = 0;
                if (!v || !parse_size(v, seed))
                {
                    std::cerr << "[Train] --seed 需要非负整数\n";
                    return false;
                }
                opt.seed = static_cast<uint64_t>(seed);
            }
            else if (a == "--patience")
            {
                const char *v = need_value("--patience");
                if (!v || !parse_int(v, opt.patience))
                {
                    std::cerr << "[Train] --patience 需要整数\n";
                    return false;
                }
            }
            else if (a == "--min-delta")
            {
                const char *v = need_value("--min-delta");
                if (!v || !parse_double(v, opt.min_delta))
                {
                    std::cerr << "[Train] --min-delta 需要数值\n";
                    return false;
                }
            }
            else if (a == "--grad-clip")
            {
                const char *v = need_value("--grad-clip");
                if (!v || !parse_double(v, opt.grad_clip))
                {
                    std::cerr << "[Train] --grad-clip 需要数值\n";
                    return false;
                }
            }
            else if (a == "--weight-decay")
            {
                const char *v = need_value("--weight-decay");
                if (!v || !parse_double(v, opt.weight_decay))
                {
                    std::cerr << "[Train] --weight-decay 需要数值\n";
                    return false;
                }
            }
            else if (a == "--device")
            {
                const char *v = need_value("--device");
                if (!v) return false;
                const std::string name = v;
                if (name == "auto")
                    opt.device = DeviceMode::Auto;
                else if (name == "cpu")
                    opt.device = DeviceMode::Cpu;
                else if (name == "gpu")
                    opt.device = DeviceMode::Gpu;
                else
                {
                    std::cerr << "[Train] --device 只支持 auto / cpu / gpu\n";
                    return false;
                }
            }
            else if (a == "--low-priority")
            {
                opt.low_priority = true;
            }
            else if (a == "--skip-bad-images")
            {
                opt.skip_bad_images = true;
            }
            else if (a == "--quiet")
            {
                opt.quiet = true;
            }
            else if (a == "--dry-run")
            {
                opt.dry_run = true;
            }
            else if (a == "--max-samples")
            {
                const char *v = need_value("--max-samples");
                if (!v || !parse_size(v, opt.max_samples))
                {
                    std::cerr << "[Train] --max-samples 需要非负整数\n";
                    return false;
                }
            }
            else if (a == "--moe-experts")
            {
                const char *v = need_value("--moe-experts");
                size_t n = 1;
                if (!v || !parse_size(v, n) || n == 0)
                {
                    std::cerr << "[Train] --moe-experts 需要 >= 1 的整数\n";
                    return false;
                }
                opt.moe_experts = n;
            }
            else if (a == "--moe-active-expert")
            {
                const char *v = need_value("--moe-active-expert");
                size_t n = 0;
                if (!v || !parse_size(v, n))
                {
                    std::cerr << "[Train] --moe-active-expert 需要非负整数\n";
                    return false;
                }
                opt.moe_active_offset = n;
            }
            else if (a == "--batch-size")
            {
                const char *v = need_value("--batch-size");
                size_t n = 1;
                if (!v || !parse_size(v, n) || n == 0)
                {
                    std::cerr << "[Train] --batch-size 需要 >= 1 的整数\n";
                    return false;
                }
                opt.batch_size = n;
            }
            else if (a == "--tile-bits")
            {
                const char *v = need_value("--tile-bits");
                size_t n = 0;
                if (!v || !parse_size(v, n) || n > 12)
                {
                    std::cerr << "[Train] --tile-bits 需要 [0,12] 的整数\n";
                    return false;
                }
                opt.tile_bits = n;
            }
            else if (a == "--tile-epsilon")
            {
                const char *v = need_value("--tile-epsilon");
                if (!v || !parse_double(v, opt.tile_epsilon) ||
                    !(opt.tile_epsilon >= 0.0) || !std::isfinite(opt.tile_epsilon))
                {
                    std::cerr << "[Train] --tile-epsilon 需要有限非负数\n";
                    return false;
                }
            }
            else if (a == "--a2l-threads")
            {
                const char *v = need_value("--a2l-threads");
                size_t n = 0;
                if (!v || !parse_size(v, n) || n == 0 || n > 256)
                {
                    std::cerr << "[Train] --a2l-threads 需要 [1,256] 的整数\n";
                    return false;
                }
                opt.a2l_threads = n;
            }
            else if (a == "--sparse-epsilon")
            {
                const char *v = need_value("--sparse-epsilon");
                if (!v || !parse_double(v, opt.sparse_epsilon) ||
                    !(opt.sparse_epsilon >= 0.0) || !std::isfinite(opt.sparse_epsilon))
                {
                    std::cerr << "[Train] --sparse-epsilon 需要有限非负数（0 = 稠密默认）\n";
                    return false;
                }
            }
            else if (a == "--sparse-tile-bits")
            {
                const char *v = need_value("--sparse-tile-bits");
                size_t n = 6;
                if (!v || !parse_size(v, n) || n > 20)
                {
                    std::cerr << "[Train] --sparse-tile-bits 需要 [0,20] 的整数\n";
                    return false;
                }
                opt.sparse_tile_bits = n;
            }
            else if (a == "--lowrank-epsilon")
            {
                const char *v = need_value("--lowrank-epsilon");
                if (!v || !parse_double(v, opt.lowrank_epsilon) ||
                    !(opt.lowrank_epsilon >= 0.0) || !std::isfinite(opt.lowrank_epsilon))
                {
                    std::cerr << "[Train] --lowrank-epsilon 需要有限非负数\n";
                    return false;
                }
            }
            else if (a == "--lowrank-rank")
            {
                const char *v = need_value("--lowrank-rank");
                size_t n = 0;
                if (!v || !parse_size(v, n) || n > 256)
                {
                    std::cerr << "[Train] --lowrank-rank 需要 [0,256] 的整数\n";
                    return false;
                }
                opt.lowrank_rank = n;
            }
            else if (a == "--state-backend")
            {
                const char *v = need_value("--state-backend");
                if (!v) return false;
                const std::string s(v);
                if (s != "auto" && s != "dense" && s != "sparse" &&
                    s != "ras" && s != "lowrank")
                {
                    std::cerr << "[Train] --state-backend 只能是 auto|dense|sparse|ras\n";
                    return false;
                }
                opt.state_backend = s;
            }
            else if (a == "--json-log")
            {
                const char *v = need_value("--json-log");
                if (!v) return false;
                opt.json_log = v;
            }
            else if (a == "--run-id")
            {
                const char *v = need_value("--run-id");
                if (!v) return false;
                opt.run_id = v;
            }
            else if (!a.empty() && a[0] == '-')
            {
                std::cerr << "[Train] 未知选项: " << a << "\n\n" << kUsage;
                return false;
            }
            else
            {
                positional.push_back(a);
            }
        }

        if (positional.size() < 2)
        {
            std::cerr << kUsage;
            return false;
        }

        opt.input_file = positional[0];
        opt.output_file = positional[1];
        if (positional.size() > 2)
        {
            if (!parse_int(positional[2], opt.epochs))
            {
                std::cerr << "[Train] 位置参数 epochs 不是整数: " << positional[2] << "\n";
                return false;
            }
        }
        if (positional.size() > 3)
        {
            if (!parse_double(positional[3], opt.lr))
            {
                std::cerr << "[Train] 位置参数 lr 不是数值: " << positional[3] << "\n";
                return false;
            }
        }
        if (positional.size() > 4)
        {
            if (!parse_int(positional[4], opt.layers))
            {
                std::cerr << "[Train] 位置参数 layers 不是整数: " << positional[4] << "\n";
                return false;
            }
            opt.layers_explicit = true;
        }
        if (positional.size() > 5)
        {
            std::cerr << "[Train] 多余的位置参数: " << positional[5] << "\n";
            return false;
        }

        return true;
    }

    class ProgressPrinter
    {
    public:
        explicit ProgressPrinter(bool quiet) : quiet_(quiet) {}

        void update(const qlm::TrainProgress &p)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!p.epoch_end && !p.stopped)
            {
                if (quiet_)
                    return;
                if (last_print_valid_ &&
                    std::chrono::duration<double>(now - last_print_).count() < 0.1)
                    return;
            }
            last_print_ = now;
            last_print_valid_ = true;

            const double total_steps =
                static_cast<double>(p.epochs) * static_cast<double>(p.total_samples);
            const double percent =
                (total_steps > 0.0)
                    ? 100.0 * static_cast<double>(p.samples_seen) / total_steps
                    : 0.0;

            std::ostringstream line;
            line << "Epoch " << p.epoch << "/" << p.epochs
                 << " | " << p.sample_in_epoch << "/" << p.total_samples
                 << " | " << std::fixed << std::setprecision(1) << percent << "%"
                 << " | loss " << std::setprecision(4) << p.epoch_loss
                 << " | acc " << std::setprecision(1) << (p.epoch_accuracy * 100.0) << "%"
                 << " | " << std::setprecision(1) << p.samples_per_sec << " samp/s"
                 << " | elapsed " << format_duration(p.elapsed_sec)
                 << " | ETA " << format_duration(p.eta_sec);

            std::string text = line.str();
            if (text.size() > 140)
                text.resize(140);

            if (!quiet_)
                std::cout << '\r';
            std::cout << text;
            if (!quiet_ && text.size() < last_width_)
                std::cout << std::string(last_width_ - text.size(), ' ');
            last_width_ = text.size();

            if (p.epoch_end || p.stopped)
            {
                std::cout << '\n';
                last_width_ = 0;
            }
            std::cout << std::flush;
        }

    private:
        bool quiet_ = false;
        bool last_print_valid_ = false;
        size_t last_width_ = 0;
        std::chrono::steady_clock::time_point last_print_{};
    };

    std::string meta_value(const std::unordered_map<std::string, std::string> &meta,
                           const std::string &key,
                           const std::string &fallback = "?")
    {
        const auto it = meta.find(key);
        return (it == meta.end()) ? fallback : it->second;
    }

    int run_training(const CliOptions &opt, bool gpu)
    {
        std::cout << "[Train] input=" << opt.input_file << " output=" << opt.output_file
                  << " epochs=" << opt.epochs << " lr=" << opt.lr
                  << " layers=" << opt.layers << " mode="
                  << (opt.image_mode ? "image+text" : "text")
                  << " optimizer=" << (opt.optimizer == qlm::Optimizer::Adam ? "adam" : "sgd")
                  << " gradient="
                  << (opt.gradient == GradientMode::ParamShift ? "param-shift" : "adjoint")
                  << " device=" << (gpu ? rvdl::accel::backend_name(rvdl::accel::compiled_backend()) : "cpu")
                  << " adjoint=" << (gpu ? "device" : "host") << "\n";

        if (opt.epochs <= 0)
            throw std::invalid_argument("[Train] epochs 必须 >= 1");
        if (!(opt.lr > 0.0))
            throw std::invalid_argument("[Train] lr 必须 > 0");
        if (opt.layers <= 0)
            throw std::invalid_argument("[Train] layers 必须 >= 1");
        if (opt.patience < 0)
            throw std::invalid_argument("[Train] patience 不能为负");
        if (!(opt.grad_clip >= 0.0) || !std::isfinite(opt.grad_clip))
            throw std::invalid_argument("[Train] grad-clip 必须是有限非负数");
        if (!(opt.weight_decay >= 0.0) || !std::isfinite(opt.weight_decay))
            throw std::invalid_argument("[Train] weight-decay 必须是有限非负数");
        if (!(opt.min_delta >= 0.0) || !std::isfinite(opt.min_delta))
            throw std::invalid_argument("[Train] min-delta 必须是有限非负数");
        if (opt.max_samples == 0)
            throw std::invalid_argument("[Train] max-samples 必须 > 0");

        const std::string run_id = opt.run_id.empty()
            ? "train-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
            : opt.run_id;
        auto log_event = [&](const std::string &type, const std::string &payload)
        {
            if (opt.json_log.empty()) return;
            std::ofstream f(opt.json_log, std::ios::app);
            if (!f) return;
            f << "{\"ts\":" << std::chrono::system_clock::now().time_since_epoch().count()
              << ",\"run_id\":\"" << run_id << "\",\"type\":\"" << type << "\""
              << payload << "}\n";
        };
        const size_t a2l_threads = opt.a2l_threads
            ? opt.a2l_threads
            : std::max<size_t>(2u, std::thread::hardware_concurrency());
        a2l::ThreadPool a2l_pool(a2l_threads);
        rvdl::sparse::SparseReport sparse_report;
        rvdl::lowrank::RasReport ras_report;
        {
            constexpr size_t kProbeChunk = 64 * 1024;
            std::vector<std::future<std::pair<uint64_t, uint64_t>>> probes;
            for (size_t b = 0; b < 4; ++b)
            {
                probes.push_back(a2l_pool.submit([path = opt.input_file, b]() -> std::pair<uint64_t, uint64_t>
                {
                    std::ifstream f(path, std::ios::binary);
                    if (!f) return {0, 0};
                    std::vector<char> buf(kProbeChunk);
                    f.seekg(std::streamoff(b) * std::streamoff(kProbeChunk), std::ios::beg);
                    f.read(buf.data(), std::streamsize(kProbeChunk));
                    const size_t got = size_t(f.gcount());
                    uint64_t ctrl = 0;
                    for (size_t i = 0; i < got; ++i)
                    {
                        const unsigned char c = static_cast<unsigned char>(buf[i]);
                        if (c < 0x09 || (c > 0x0d && c < 0x20)) ++ctrl;
                    }
                    return {ctrl, uint64_t(got)};
                }));
            }
            uint64_t ctrl_total = 0, bytes_total = 0;
            for (auto &p : probes) { const auto r = p.get(); ctrl_total += r.first; bytes_total += r.second; }
            (void)ctrl_total;

            rvdl::sparse::SparseConfig scfg;
            scfg.epsilon = opt.sparse_epsilon;
            scfg.tile_bits = opt.sparse_tile_bits ? opt.sparse_tile_bits : 6;
            scfg.cpu_threads = a2l_threads;
            const size_t probe_dim = size_t(1) << std::min<size_t>(size_t(std::max(1, opt.qubits)), 20);
            rvdl::sparse::State probe(probe_dim, rvdl::sparse::Complex(1.0, 0.0));
            const double inv = 1.0 / std::sqrt(double(probe_dim));
            for (auto &z : probe) z *= inv;
            sparse_report = rvdl::sparse::evaluate(probe, size_t(std::max(1, opt.qubits)), scfg);
            const std::string backend = sparse_report.backend == rvdl::sparse::BackendChoice::Dense ? "dense" : "dq_sparse";
            auto ras_future = a2l_pool.submit([&]() -> rvdl::lowrank::RasReport
            {
                rvdl::lowrank::RasReport out;
                if (opt.lowrank_epsilon <= 0.0 || opt.lowrank_rank == 0)
                {
                    out.reason = "RAS disabled (epsilon=0 or rank=0)";
                    return out;
                }
                rvdl::lowrank::RasConfig rc;
                rc.tile_bits = opt.sparse_tile_bits ? opt.sparse_tile_bits : 6;
                rc.max_rank = opt.lowrank_rank;
                rc.epsilon = opt.lowrank_epsilon;
                rc.seed = opt.seed;
                const size_t pd = size_t(1) << std::min<size_t>(size_t(std::max(1, opt.qubits)), 16);
                rvdl::lowrank::State ps(pd, rvdl::lowrank::Complex(0.0, 0.0));
                for (size_t i = 0; i < pd; ++i)
                    ps[i] = rvdl::lowrank::Complex(
                        1.0 / std::sqrt(double(pd)) *
                        (1.0 + 0.05 * std::sin(0.07 * double(i))), 0.0);
                const double nrm = std::sqrt(std::accumulate(ps.begin(), ps.end(), 0.0,
                    [](double a, const rvdl::lowrank::Complex &z) { return a + std::norm(z); }));
                for (auto &z : ps) z /= nrm;
                rvdl::lowrank::LowRankState ras(ps, rc, &out);
                out.reason += " | measured=" + std::to_string(ras.measured_error(ps));
                return out;
            });
            ras_report = ras_future.get();

            log_event("a2l", ",\"threads\":" + std::to_string(a2l_threads) +
                      ",\"probe_bytes\":" + std::to_string(bytes_total) +
                      ",\"sparse_backend\":\"" + backend + "\"" +
                      ",\"ras_rank\":" + std::to_string(ras_report.rank) +
                      ",\"ras_compression\":" + std::to_string(ras_report.compression));
            if (!opt.quiet)
                std::cout << "[Train] A2L threads=" << a2l_threads
                          << " sparse=" << backend
                          << " reason=" << sparse_report.reason
                          << " | RAS rank=" << ras_report.rank
                          << " compress=" << ras_report.compression << "x"
                          << " bound=" << ras_report.error_bound << "\n";
        }

        log_event("config", ",\"input\":\"" + opt.input_file + "\",\"output\":\"" + opt.output_file +
                  "\",\"epochs\":" + std::to_string(opt.epochs) +
                  ",\"lr\":" + std::to_string(opt.lr) +
                  ",\"layers\":" + std::to_string(opt.layers) +
                  ",\"seed\":" + std::to_string(opt.seed) +
                  ",\"limit\":" + std::to_string(opt.limit) +
                  ",\"max_samples\":" + std::to_string(opt.max_samples));

        uint64_t data_bytes = 0;
        std::string data_fingerprint;
        if (!opt.image_mode)
        {
            const auto check = qlm::training::validate_training_text(opt.input_file);
            if (!check.error.empty())
                throw std::runtime_error("[Train] 训练数据预检失败: " + check.error);
            data_bytes = (opt.limit > 0 && check.bytes > opt.limit) ? opt.limit : check.bytes;
            uint64_t fp = 14695981039346656037ull;
            std::ifstream head(opt.input_file, std::ios::binary);
            char buf[4096] = {0};
            if (head)
            {
                head.read(buf, static_cast<std::streamsize>(sizeof(buf)));
                for (std::streamsize i = 0; i < head.gcount(); ++i)
                {
                    fp ^= static_cast<unsigned char>(buf[i]);
                    fp *= 1099511628211ull;
                }
            }
            std::ostringstream fss;
            fss << std::hex << fp;
            data_fingerprint = fss.str();
        }
        if (opt.dry_run)
        {
            std::cout << "[Train] dry-run: run_id=" << run_id
                      << " input=" << opt.input_file
                      << " bytes=" << data_bytes
                      << " limit=" << opt.limit
                      << " max_samples=" << opt.max_samples
                      << " fingerprint=" << data_fingerprint
                      << "\n[Train] dry-run: 数据预检通过，未训练、未写模型。\n";
            log_event("dry_run", ",\"bytes\":" + std::to_string(data_bytes) +
                      ",\"fingerprint\":\"" + data_fingerprint + "\"");
            return 0;
        }
        if (!opt.checkpoint_path.empty() && opt.checkpoint_every == 0)
            std::cout << "[Train] checkpoint: 每个 epoch 末保存到 " << opt.checkpoint_path << "\n";
        else if (!opt.checkpoint_path.empty())
            std::cout << "[Train] checkpoint: 每 " << opt.checkpoint_every
                      << " 个样本保存到 " << opt.checkpoint_path << "\n";
        else if (opt.checkpoint_every > 0)
            std::cout << "[Train] 警告: 指定了 --checkpoint-every 但没有 --checkpoint，检查点被忽略\n";

        std::string image_root = opt.image_root;
        if (opt.image_mode && image_root.empty())
        {
            image_root = std::filesystem::path(opt.input_file).parent_path().string();
            if (image_root.empty())
                image_root = ".";
        }

        qlm::training::MultiFileTextCorpus text_corpus;
        qlm::training::CaptionCorpus caption_corpus;
        std::function<bool(size_t, qlm::training::SampleView &)> fetch;

        if (opt.image_mode)
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::cout << "[Train] image-root=" << image_root
                      << " limit=" << (opt.limit ? std::to_string(opt.limit) : std::string("all"))
                      << "（正在扫描 caption 并解码图像...）\n" << std::flush;
            caption_corpus.load(opt.input_file, image_root, opt.limit,
                                load_image_angles, opt.skip_bad_images);
            const double scan_sec =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "[Train] captions=" << caption_corpus.caption_count()
                      << " images=" << caption_corpus.image_count()
                      << " skipped_images=" << caption_corpus.skipped_images().size()
                      << " samples=" << caption_corpus.total_samples()
                      << " scan=" << format_duration(scan_sec) << "\n";
            if (caption_corpus.empty())
                throw std::runtime_error("[Train] 没有可用训练样本（caption 过短或全部被跳过）");
            auto *source = &caption_corpus;
            fetch = [source](size_t index, qlm::training::SampleView &out)
            {
                return source->get(index, out);
            };
        }
        else
        {
            text_corpus.add_file(opt.input_file, opt.limit);
            for (const auto &skipped : text_corpus.skipped_files())
                std::cout << "[Train] 跳过过短文件: " << skipped << "\n";
            std::cout << "[Train] samples=" << text_corpus.total_samples()
                      << "（按字节 token 计，上下文窗口按 qubit 数推导）\n";
            if (text_corpus.empty())
                throw std::runtime_error(
                    "[Train] 训练文件不足 3 个字节，没有可学习样本: " + opt.input_file);
            auto *source = &text_corpus;
            fetch = [source](size_t index, qlm::training::SampleView &out)
            {
                return source->get(index, out);
            };
        }

        const size_t total_samples =
            opt.image_mode ? caption_corpus.total_samples() : text_corpus.total_samples();

        if (total_samples > opt.max_samples)
            throw std::runtime_error(
                "[Train] 样本数 " + std::to_string(total_samples) + " 超过 --max-samples " +
                std::to_string(opt.max_samples) + "；请用 --limit <字节数> 切一个受控子集");
        log_event("data", ",\"samples\":" + std::to_string(total_samples) +
                  ",\"bytes\":" + std::to_string(data_bytes) +
                  ",\"fingerprint\":\"" + data_fingerprint + "\"");

        int effective_qubits = opt.qubits;
        if (opt.qubits_auto)
            effective_qubits = static_cast<int>(qlm::training::max_training_qubits());

        std::vector<double> init_weights;
        const std::vector<double> *init_ptr = nullptr;
        int effective_layers = opt.layers;
        if (!opt.continue_path.empty())
        {
            qml::RVDLModel<double> loaded = [&]()
            {
                try
                {
                    return qml::ModelExporter<double>::load(opt.continue_path);
                }
                catch (const std::exception &e)
                {
                    throw std::runtime_error(
                        "[Train] 无法加载 --continue 模型 '" + opt.continue_path + "': " +
                        e.what());
                }
            }();

            init_weights.assign(loaded.tensor.data(),
                                loaded.tensor.data() + loaded.tensor.size());

            const std::string model_qubits_str =
                meta_value(loaded.metadata, "train.qubits", "16");
            int model_qubits = 0;
            if (!parse_int(model_qubits_str, model_qubits) ||
                model_qubits < static_cast<int>(qlm::training::kMinTrainingQubits) ||
                model_qubits > static_cast<int>(qlm::training::kMaxTrainingQubits))
                throw std::runtime_error(
                    "[Train] --continue 模型元数据 train.qubits 无效: '" +
                    model_qubits_str + "'（支持 8..20）");

            if (!opt.qubits_explicit)
            {
                effective_qubits = model_qubits;
                std::cout << "[Train] 从模型元数据推断 qubits="
                          << effective_qubits << "\n";
            }
            else if (effective_qubits != model_qubits)
            {
                throw std::runtime_error(
                    "[Train] --qubits " + std::to_string(effective_qubits) +
                    " 与 --continue 模型的 train.qubits=" +
                    std::to_string(model_qubits) +
                    " 不一致；去掉 --qubits 让训练器自动推断。");
            }

            const size_t model_ppl =
                params_per_layer(static_cast<size_t>(model_qubits));
            const size_t params = init_weights.size();
            if (params == 0 || params % model_ppl != 0)
                throw std::runtime_error(
                    "[Train] --continue 模型参数数 " + std::to_string(params) +
                    " 不是 " + std::to_string(model_ppl) +
                    " 的整数倍，无法对应 " + std::to_string(model_qubits) +
                    "-qubit QLM 网络");

            const int model_layers = static_cast<int>(params / model_ppl);
            if (!opt.layers_explicit)
            {
                effective_layers = model_layers;
                std::cout << "[Train] 从模型元数据推断 layers=" << effective_layers << "\n";
            }
            else if (effective_layers != model_layers)
            {
                throw std::runtime_error(
                    "[Train] --continue 模型有 " + std::to_string(params) +
                    " 个参数（" + std::to_string(model_layers) + " 层），但你指定了 --layers " +
                    std::to_string(effective_layers) + "（需要 " +
                    std::to_string(effective_layers * static_cast<int>(model_ppl)) +
                    " 个参数）。去掉 --layers 让训练器自动推断，或换匹配的旧模型。");
            }

            init_ptr = &init_weights;
            std::cout << "[Train] continue from " << opt.continue_path
                      << " (" << init_weights.size() << " params, "
                      << model_layers << " layers, "
                      << model_qubits << " qubits)\n";

            if (loaded.metadata.find("model.ansatz") == loaded.metadata.end())
                std::cout << "[Train] 警告: 该模型没有 model.ansatz 标记，很可能是旧版 "
                             "RZ-only ansatz（读出与参数无关，无法学习）。继续训练会把旧权重"
                             "当作 RY 参数使用；建议从零训练。\n";
        }

        if (effective_qubits < static_cast<int>(qlm::training::kMinTrainingQubits) ||
            effective_qubits > static_cast<int>(qlm::training::kMaxTrainingQubits))
            throw std::invalid_argument(
                "[Train] qubits 必须在 " +
                std::to_string(qlm::training::kMinTrainingQubits) + ".." +
                std::to_string(qlm::training::kMaxTrainingQubits) +
                " 之间（当前 " + std::to_string(effective_qubits) + "）");

        const size_t effective_ppl =
            params_per_layer(static_cast<size_t>(effective_qubits));
        const auto ctx_cfg =
            qlm::lf2c2::context_config_for_qubits(static_cast<size_t>(effective_qubits));

        std::cout << "[Train] qubits=" << effective_qubits
                  << " layers=" << effective_layers
                  << " params=" << (effective_layers * static_cast<int>(effective_ppl))
                  << " samples=" << total_samples
                  << " context_window=" << ctx_cfg.window_tokens
                  << (opt.shuffle ? " shuffle=on" : "")
                  << (opt.patience > 0 ? (" patience=" + std::to_string(opt.patience)) : "")
                  << "\n";
        std::cout << "[Train] Ctrl+C：当前样本结束后安全停止并保存；再按一次强制退出。\n"
                  << std::flush;

        qhal::QVM vm;
        vm.allocate_qubits(static_cast<size_t>(effective_qubits));
        global_qm = &vm;
        qlm::QLM lm(&vm, static_cast<size_t>(effective_qubits),
                    static_cast<size_t>(effective_layers));

        std::vector<size_t> qubit_ids(static_cast<size_t>(effective_qubits));
        std::iota(qubit_ids.begin(), qubit_ids.end(), size_t(0));

        qlm::training::SampleView current_view;
        uint8_t current_target = 0;

        auto sample = std::make_shared<rvdl::QDataState>(&vm, qubit_ids, false);
        sample->qlm_data = &current_target;

        const auto get_sample = [&](size_t index) -> std::shared_ptr<rvdl::QObject>
        {
            if (!fetch(index, current_view))
                throw std::runtime_error(
                    "[Train] 数据源读取失败：样本索引 " + std::to_string(index) +
                    "（总数 " + std::to_string(total_samples) + "）");
            current_target = current_view.target;
            return sample;
        };

        const auto prepare_sample = [&](std::shared_ptr<rvdl::QObject> &)
        {
            const size_t n_q = static_cast<size_t>(effective_qubits);
            std::vector<double> angles = qlm::lf2c2::context_angles(
                ctx_cfg, current_view.window, n_q);
            angles.resize(n_q, 0.0);

            if (current_view.has_image)
            {
                for (size_t j = 0; j < kReadoutBits; ++j)
                    angles[j] += current_view.image_angles[j];
            }

            const auto amps = qlm::training::product_state_amplitudes(angles, n_q);
            vm.prepare_amplitudes(amps, n_q);
        };

        std::unordered_map<std::string, std::string> extra_meta;
        extra_meta["train.interface"] = "train_cli";
        extra_meta["train.a2l_threads"] = std::to_string(a2l_threads);
        extra_meta["train.probe_backend"] =
            sparse_report.backend == rvdl::sparse::BackendChoice::Dense ? "dense" : "dq_sparse";
        extra_meta["train.probe_reason"] = sparse_report.reason;
        extra_meta["train.probe_epsilon"] = std::to_string(opt.sparse_epsilon);
        extra_meta["train.probe_error_bound"] = std::to_string(sparse_report.error_bound);
        extra_meta["train.ras_enabled"] = (opt.lowrank_epsilon > 0.0 && opt.lowrank_rank > 0) ? "1" : "0";
        extra_meta["train.ras_rank"] = std::to_string(ras_report.rank);
        extra_meta["train.ras_compression"] = std::to_string(ras_report.compression);
        extra_meta["train.ras_delta"] = std::to_string(ras_report.delta);
        extra_meta["train.ras_error_bound"] = std::to_string(ras_report.error_bound);
        extra_meta["train.moe_experts"] = std::to_string(std::max<size_t>(1, opt.moe_experts));
        extra_meta["train.input"] = std::filesystem::path(opt.input_file).filename().string();
        extra_meta["train.fast_prepare"] = "product_state_amplitudes";
        extra_meta["train.run_id"] = run_id;
        extra_meta["train.seed"] = std::to_string(opt.seed);
        extra_meta["train.shuffle"] = opt.shuffle ? "1" : "0";
        extra_meta["train.grad_clip"] = std::to_string(opt.grad_clip);
        extra_meta["train.weight_decay"] = std::to_string(opt.weight_decay);
        extra_meta["train.early_stop_patience"] = std::to_string(opt.patience);
        extra_meta["train.limit_bytes"] = std::to_string(opt.limit);
        extra_meta["train.max_samples"] = std::to_string(opt.max_samples);
        extra_meta["train.data_bytes"] = std::to_string(data_bytes);
        extra_meta["train.data_fingerprint"] = data_fingerprint;
        extra_meta["train.data_fingerprint_algo"] = "fnv1a-head-4096";
        if (opt.image_mode)
        {
            extra_meta["train.source"] = "caption_jsonl";
            extra_meta["mm.modality"] = "image+text";
            extra_meta["mm.image_encoder"] = "stb_gray_4x2_bandmean_RY";
            extra_meta["mm.image_qubits"] = "0-7";
            extra_meta["mm.fusion"] = "RY_angle_sum_with_LF2C2_context";
            extra_meta["mm.dataset"] = std::filesystem::path(opt.input_file).filename().string();
        }
        else
        {
            extra_meta["train.source"] = "text";
        }

        qlm::TrainOptions options;
        {
            const rvdl::accel::DeviceInfo dev =
                rvdl::accel::select(opt.device != DeviceMode::Cpu, false);
            qlm::StateResources &sr = options.state_resources;
            sr.qubits = static_cast<size_t>(std::max(1, opt.qubits));
            sr.batch_size = std::max<size_t>(1, opt.batch_size);
            sr.params = static_cast<size_t>(std::max(1, opt.layers)) * (sr.qubits + 8);
            sr.host_memory_bytes = qlm::training::available_memory_bytes();
            sr.device_memory_bytes = gpu ? dev.memory_bytes : 0;
            sr.cpu_threads = a2l_threads;
            sr.gpu_available = gpu;
            sr.moe_experts = std::max<size_t>(1, opt.moe_experts);
            sr.sparse_tile_bits = opt.sparse_tile_bits ? opt.sparse_tile_bits : 6;
            sr.requested_sparse_epsilon = opt.sparse_epsilon;
        }
        options.state_backend = qlm::parse_state_backend(opt.state_backend);
        options.sparse_epsilon = opt.sparse_epsilon;
        options.sparse_tile_bits = opt.sparse_tile_bits ? opt.sparse_tile_bits : 6;
        options.ras_rank = opt.lowrank_rank;
        options.ras_epsilon = opt.lowrank_epsilon > 0.0 ? opt.lowrank_epsilon : 0.0;
        options.ras_seed = opt.seed;
        if (options.state_backend == qlm::StateBackend::Ras)
            options.device_adjoint_sparse_batch_pod = gpu
                ? &rvdl::ras_device::ras_device_batch_pod
                : &rvdl::ras_adjoint::ras_adjoint_batch_pod;
        else if (gpu)
            options.device_adjoint_sparse_batch_pod = &rvdl::accel::device_adjoint_sparse_batch_pod;
        else
            options.device_adjoint_sparse_batch_pod = &rvdl::sparse_adjoint::sparse_adjoint_batch_pod;
        if (gpu)
            options.device_adjoint_pod = &rvdl::accel::device_adjoint_pod;
        options.moe_experts = std::max<size_t>(1, opt.moe_experts);
        options.moe_active_offset = opt.moe_active_offset;
        size_t bat = std::max<size_t>(1, opt.batch_size);
        if (options.moe_experts > 1 && bat > 1)
        {
            std::cout << "[Train] MoE 逐样本专家路由与 BAT 共享酉不兼容，"
                      << "--batch-size 自动降为 1（改用 --moe-experts 稀疏路径）。\n";
            bat = 1;
        }
        options.batch_size = bat;
        options.tile_bits = opt.tile_bits;
        options.tile_epsilon = opt.tile_epsilon;
        if (gpu && bat > 1)
            options.device_adjoint_batch_pod = &rvdl::accel::device_adjoint_batch_pod;
        options.epochs = opt.epochs;
        options.lr = opt.lr;
        options.optimizer = opt.optimizer;
        options.weight_decay = opt.weight_decay;
        options.grad_clip = opt.grad_clip;
        options.early_stop_patience = opt.patience;
        options.early_stop_min_delta = opt.min_delta;
        options.init_weights = init_ptr;
        options.prepare_sample = prepare_sample;
        options.shuffle = opt.shuffle;
        options.seed = opt.seed;
        options.use_adjoint = (opt.gradient != GradientMode::ParamShift);
        options.extra_meta = std::move(extra_meta);
        options.progress_every = 0;

        options.on_epoch_start = [&](int, size_t)
        {
            if (opt.image_mode)
                caption_corpus.rewind();
            else
                text_corpus.rewind();
        };

        ProgressPrinter printer(opt.quiet);
        options.progress = [&](const qlm::TrainProgress &p)
        {
            printer.update(p);
        };

        options.should_stop = []()
        {
            return g_sigint_count.load(std::memory_order_relaxed) > 0;
        };

        if (!opt.checkpoint_path.empty())
        {
            options.checkpoint_every = opt.checkpoint_every;
            options.checkpoint =
                [&](const numqk::Tensor<double> &theta, const qlm::TrainProgress &p)
            {
                std::unordered_map<std::string, std::string> meta;
                meta["model.architecture"] = "QQNT";
                meta["topology.type"] =
                    "Cross_Attention_" + std::to_string(effective_qubits) + "Q";
                meta["model.ansatz"] = "RY_RZ_CNOT_v3";
                meta["model.param_layout"] =
                    "per_layer:RY(n_q)+CNOT(ctx->readout)+RZ(8)";
                meta["model.readout_base"] =
                    std::to_string(effective_qubits - static_cast<int>(kReadoutBits));
                meta["context.window"] = "LF2C_" +
                                         std::to_string(ctx_cfg.window_tokens) +
                                         "Tokens_" +
                                         std::to_string(effective_qubits) + "Q";
                meta["context.encoder"] = "LF2C2";
                meta["checkpoint.partial"] = "1";
                meta["checkpoint.epoch"] = std::to_string(p.epoch);
                meta["checkpoint.epochs"] = std::to_string(p.epochs);
                meta["checkpoint.sample_in_epoch"] = std::to_string(p.sample_in_epoch);
                meta["checkpoint.samples_seen"] = std::to_string(p.samples_seen);
                meta["checkpoint.loss"] = std::to_string(p.epoch_loss);
                meta["checkpoint.accuracy"] = std::to_string(p.epoch_accuracy);
                meta["checkpoint.elapsed_sec"] = std::to_string(p.elapsed_sec);
                meta["train.interface"] = "train_cli";
                meta["train.optimizer"] =
                    (opt.optimizer == qlm::Optimizer::Adam) ? "adam" : "sgd";
                meta["train.learning_rate"] = std::to_string(opt.lr);
                meta["train.qubits"] = std::to_string(effective_qubits);
                meta["train.layers"] = std::to_string(effective_layers);
                meta["train.params"] = std::to_string(theta.size());
                meta["train.input"] =
                    std::filesystem::path(opt.input_file).filename().string();
                if (opt.image_mode)
                {
                    meta["mm.modality"] = "image+text";
                    meta["mm.image_encoder"] = "stb_gray_4x2_bandmean_RY";
                    meta["mm.image_qubits"] = "0-7";
                    meta["mm.fusion"] = "RY_angle_sum_with_LF2C2_context";
                }

                qml::RVDLModel<double> checkpoint_model(theta, meta);
                qml::ModelExporter<double>::save_atomic(opt.checkpoint_path, checkpoint_model);
                std::cout << "[Train] checkpoint saved: " << opt.checkpoint_path
                          << " (epoch " << p.epoch << "/" << p.epochs
                          << ", sample " << p.samples_seen << ")\n" << std::flush;
            };
        }

        const auto t_start = std::chrono::steady_clock::now();
        qml::RVDLModel<double> model =
            lm.train_stream(opt.output_file, total_samples, get_sample, options);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

        std::cout << "[Train] done: " << opt.output_file
                  << " params=" << model.tensor.size()
                  << " epochs_completed=" << meta_value(model.metadata, "train.epochs_completed")
                  << " best_epoch=" << meta_value(model.metadata, "train.best_epoch")
                  << " best_loss=" << meta_value(model.metadata, "train.best_loss")
                  << " last_loss=" << meta_value(model.metadata, "train.last_loss")
                  << " last_acc=" << meta_value(model.metadata, "train.last_accuracy")
                  << " gradient=" << meta_value(model.metadata, "train.gradient")
                  << " duration=" << format_duration(elapsed)
                  << (meta_value(model.metadata, "train.stopped", "0") == "1" ? " stopped=1" : "")
                  << "\n";
        log_event("done", ",\"params\":" + std::to_string(model.tensor.size()) +
                  ",\"elapsed_sec\":" + std::to_string(elapsed) +
                  ",\"best_loss\":\"" + meta_value(model.metadata, "train.best_loss") + "\"" +
                  ",\"output\":\"" + opt.output_file + "\"");

        if (g_sigint_count.load() > 0)
            std::cout << "[Train] 已按 Ctrl+C 请求在样本边界停止；当前权重已保存。\n";

        return 0;
    }

}

int main(int argc, char **argv)
{
    CliOptions opt;
    if (!parse_args(argc, argv, opt))
        return 2;
    if (opt.show_help)
    {
        std::cout << kUsage;
        return 0;
    }

    install_signal_handlers(true);
    disable_console_quick_edit();
    if (opt.low_priority)
    {
        lower_process_priority();
        std::cout << "[Train] 进程优先级已降到 BelowNormal\n";
    }

    if (!std::getenv("OMP_NUM_THREADS"))
        omp_set_num_threads(1);

    const char *nogpu = std::getenv("RVDL_NO_GPU");
    const bool disabled_by_env = nogpu && nogpu[0] && std::strcmp(nogpu, "0") != 0 &&
                                 std::strcmp(nogpu, "false") != 0 &&
                                 std::strcmp(nogpu, "no") != 0 && std::strcmp(nogpu, "off") != 0;
    const rvdl::accel::Detection detection = rvdl::accel::detect();
    rvdl::accel::DeviceInfo dev = rvdl::accel::select(opt.device != DeviceMode::Cpu, false);
    if (disabled_by_env) { dev.usable = false; dev.reason = "RVDL_NO_GPU 已设置，强制 CPU/OpenMP"; }
    if (opt.device == DeviceMode::Cpu) { dev.usable = false; dev.reason = "--device cpu：使用 CPU/OpenMP"; }
    const bool gpu = dev.usable;

    rvdl::set_gpu_backend_available(gpu);
    rvdl::accel::set_device_ready(gpu);
    std::cout << "[Train] device: " << rvdl::accel::describe(dev) << "\n";
    for (const auto &other : detection.others)
        std::cout << "[Train]   also present: " << rvdl::accel::vendor_name(other.vendor)
                  << " x" << other.count << " (本二进制未编该后端，忽略)\n";

    if (opt.device == DeviceMode::Gpu && !gpu)
    {
        std::cerr << "[Train] 指定了 --device gpu，但 " << dev.reason << "\n";
        return 1;
    }
    if (gpu)
    {
        std::cout << "[Train] QLM adjoint → device kernel (" << rvdl::accel::backend_name(dev.backend)
                  << " / " << rvdl::accel::execution_space_name()
                  << ")；QVM 门 / 批量 ⟨Z⟩ / shot 采样同后端。\n";
    }
    else
    {
        std::cout << "[Train] QLM adjoint → host 稠密态（OpenMP/Serial）。\n";
    }

    Kokkos::initialize(argc, argv);

    int rc = 0;
    try
    {
        rc = run_training(opt, gpu);
        const auto calls = rvdl::accel::adjoint_device_calls().load();
        if (calls > 0)
        {
            const double total_ms = rvdl::accel::adjoint_device_us().load() / 1000.0;
            std::cout << "[Train] accel: device_adjoint calls=" << calls
                      << " total=" << total_ms << "ms avg=" << (total_ms / calls) << "ms\n";
        }
    }
    catch (const std::exception &e)
    {
        const std::string m = e.what();
        std::cerr << "\n[Train] 训练失败: " << m << "\n";
        rc = 1;
        if (m.find("预检失败") != std::string::npos ||
            m.find("样本数") != std::string::npos ||
            m.find("训练文件") != std::string::npos) rc = 3;
        else if (m.find("nan") != std::string::npos ||
                 m.find("NaN") != std::string::npos ||
                 m.find("非有限") != std::string::npos) rc = 4;
        else if (m.find("Hardware Error") != std::string::npos ||
                 m.find("Format Error") != std::string::npos ||
                 m.find("cannot open") != std::string::npos) rc = 5;
        else if (m.find("必须") != std::string::npos ||
                 m.find("需要") != std::string::npos) rc = 2;
        if (!opt.json_log.empty())
        {
            std::ofstream f(opt.json_log, std::ios::app);
            if (f) f << "{\"type\":\"error\",\"code\":" << rc
                     << ",\"message\":\"" << m << "\"}\n";
        }
    }
    catch (...)
    {
        std::cerr << "\n[Train] 训练失败: 未知异常\n";
        rc = 1;
    }

    Kokkos::finalize();
    return rc;
}