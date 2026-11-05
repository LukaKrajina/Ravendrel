#pragma once
// ============================================================
// A2a.hpp —— Android 特化优化（桌面/其它平台上一律惰性）
//
// Android 需要**另一套**策略是有原因的。
// ---------------------------------
// 同一条渲染路径，桌面和手机的代价模型不同：
//
//   1. **发热是硬约束**。桌面可以一直全速；手机持续全速会降频，表现是"越用越卡"。
//      Android 的 thermal 等级不是建议，是"再热下去系统会杀你的进程"。
//   2. **大小核**（big.LITTLE / DynamIQ）。渲染线程被调度到小核上，就会和后台
//      任务抢时间片 —— 帧率抖动看起来像"引擎问题"，其实是调度问题。
//   3. **TBR**（tile-based rendering）。移动 GPU 上 `LOAD_OP_CLEAR` / `STORE_OP_DONT_CARE`
//      / `LAZILY_ALLOCATED` 直接等于带宽和电量；桌面上的"无所谓"在这里是主要开销。
//   4. **预旋转与帧节奏**。`currentTransform` 由合成器做旋转要花电；在正确的时间
//      呈现（Swappy / `VK_GOOGLE_display_timing`）决定了"跟手"与否。
//
// 所以五条设计约定
// ------------
// 1. **不在 Android 上就是惰性的**：`available()` 返回 false，所有探测返回 -1 /
//    空值。理由和 fox 的剪贴板一致 —— "没有这个东西"必须是一个**可判定的值**，
//    而不是"一个会静默返回垃圾的桩"。
// 2. **探测与决策分离**。这一层只做**可判定**的事：读 sysfs / 解析字符串 / 查表。
//    需要宿主上下文才能知道的事（显示旋转、是否支持 display timing、GPU 是不是
//    TBR）由宿主**报告**进来（`report_*`），A2a 不猜。猜出来的旋转会画出错误
//    方向的画面，而且不会有任何报错。
// 3. **降级的顺序是设计的一部分**：先降玻璃模糊 → 再过降阴影 → 最后才降帧率。
//    帧率是用户最直接感知的东西，绝不能是第一个被牺牲的。这条顺序由测试钉住。
// 4. **阈值是启发式，不是规范**。thermal 等级的温度阈值可配置，默认值写在
//    `ThermalThresholds` 里并标注来源（设备厂商各不相同）。**不声称**这些数字是
//    Android 官方标准 —— 官方标准是 PowerManager 的 thermal status，那一层需要
//    JNI/Java 侧，NDK 的纯 C++ 拿不到。
// 5. **测试不需要真机**：所有解析都接受一个"根目录"参数，测试用 fixture 目录
//    覆盖真实 sysfs 的形状（含读失败、缺字段、多簇、异常值）。"只在真机上可测"
//    等于"永远不被测"。
//
// 本文件不 include 任何 Vulkan / fox 头：它输出的是**策略值**（含 Vulkan 枚举的
// 数值常量，作为普通 int），由后端自己翻译。这样它能在没有 Vulkan 的机器上被测。
// ============================================================

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace a2a
{
    // ── 平台判定 ─────────────────────────────────────────────────────

#if defined(__ANDROID__)
    constexpr bool is_android_build = true;
#else
    constexpr bool is_android_build = false;
#endif

    /// sysfs / procfs 的根。测试用 fixture 目录替换它。
    ///
    /// ⚠ 这是"可注入"的设计，不是给生产代码用的开关：生产路径永远用默认值，
    ///   而测试可以完整覆盖真实 sysfs 的形状（见文件头第 5 条）。
    struct Paths
    {
        std::string thermal = "/sys/class/thermal";
        std::string cpu = "/sys/devices/system/cpu";
        std::string cpuinfo = "/proc/cpuinfo";
        std::string thermal_policy = "/sys/devices/virtual/thermal";
    };

    inline const Paths &default_paths()
    {
        static const Paths p;
        return p;
    }

    // ── 热状态 ───────────────────────────────────────────────────────

    enum class Tier : int
    {
        Unknown = -1,
        Normal = 0,
        Light = 1,
        Moderate = 2,
        Severe = 3,
        Critical = 4,
        Emergency = 5,
        Shutdown = 6,
    };

    inline const char *tier_name(Tier t)
    {
        switch (t)
        {
        case Tier::Unknown: return "unknown";
        case Tier::Normal: return "normal";
        case Tier::Light: return "light";
        case Tier::Moderate: return "moderate";
        case Tier::Severe: return "severe";
        case Tier::Critical: return "critical";
        case Tier::Emergency: return "emergency";
        case Tier::Shutdown: return "shutdown";
        }
        return "?";
    }

    /// 温度阈值（毫摄氏度）。**默认值是可配置的启发式**，来源是常见 SoC 的
    /// thermal 配置（厂商各不相同，差距能到 ±10°C）—— 见文件头第 4 条。
    struct ThermalThresholds
    {
        int light = 60'000;
        int moderate = 70'000;
        int severe = 80'000;
        int critical = 85'000;
        int emergency = 90'000;
        int shutdown = 95'000;
    };

    inline Tier tier_of_temp(int temp_mc, const ThermalThresholds &th = {})
    {
        if (temp_mc <= 0) return Tier::Unknown;   // 未读到
        if (temp_mc >= th.shutdown) return Tier::Shutdown;
        if (temp_mc >= th.emergency) return Tier::Emergency;
        if (temp_mc >= th.critical) return Tier::Critical;
        if (temp_mc >= th.severe) return Tier::Severe;
        if (temp_mc >= th.moderate) return Tier::Moderate;
        if (temp_mc >= th.light) return Tier::Light;
        return Tier::Normal;
    }

    struct ThermalState
    {
        bool available = false;       ///< 至少成功读到过一个 SoC 相关 zone
        Tier tier = Tier::Unknown;
        int worst_temp_mc = -1;       ///< 最热 SoC zone 的温度（毫摄氏度）
        int zones_seen = 0;           ///< 目录里出现的 zone 数（含读失败）
        int zones_used = 0;           ///< 参与判定的 SoC zone 数
        int zones_failed = 0;         ///< 打开/解析失败的 zone 数
        std::string worst_zone;       ///< 最热 zone 的 type（"cpu-0-0" 之类）
    };

    /// 哪些 zone 算"SoC 相关"。
    ///
    /// 不加过滤的话，`battery`/`charger` 这类 zone 会在充电时把等级顶到
    /// Critical —— 于是渲染质量因为"电池发热"被降到最低，而 SoC 其实很凉。
    /// 过滤是**判定**：包含 SoC/CPU/GPU/tsens 关键词，排除电池/充电/屏幕。
    inline bool zone_is_soc(const std::string &type)
    {
        static const char *inc[] = {"cpu", "gpu", "soc", "tsens", "apc", "big", "little",
                                    "mid",    "core", "pmic", "board", "nsp", "disp"};
        static const char *exc[] = {"battery", "charger", "usb", "pa", "screen", "display",
                                    "backlight", "flash", "camera", "wifi", "modem", "skin"};
        std::string t;
        t.resize(type.size());
        for (size_t i = 0; i < type.size(); ++i) t[i] = (char)std::tolower((unsigned char)type[i]);
        for (const char *e : exc)
            if (t.find(e) != std::string::npos) return false;
        for (const char *i : inc)
            if (t.find(i) != std::string::npos) return true;
        return false;
    }

    namespace detail
    {
        inline bool read_text_file(const std::string &path, std::string &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::string line;
            std::getline(in, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            out = line;
            return true;
        }

        inline bool parse_int(const std::string &s, long long &out)
        {
            if (s.empty()) return false;
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            bool neg = false;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; ++i; }
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            long long v = 0;
            for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
            out = neg ? -v : v;
            return true;
        }

#if defined(__linux__) || defined(__ANDROID__)
        inline bool list_dir(const std::string &path, std::vector<std::string> &names, bool sorted = true)
        {
            DIR *d = ::opendir(path.c_str());
            if (!d) return false;
            names.clear();
            while (dirent *e = ::readdir(d))
            {
                const std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                names.push_back(n);
            }
            ::closedir(d);
            if (sorted) std::sort(names.begin(), names.end());
            return true;
        }
#endif
    } // namespace detail

    /// 读 thermal 状态。纯文件解析 —— 任何平台都能在 fixture 上跑。
    ///
    /// temp 文件是**毫摄氏度**（`45000` = 45°C）。有些老设备/错误的驱动写的是
    /// 整摄氏度（`45`），把它当毫摄氏度就会得出"永远不热"的假象 —— 热降级整条
    /// 链路静默失效，且不会有任何报错。所以这里做量级归一（见下），并且明确
    /// 它只处理这一种可判定的情况。
    inline ThermalState read_thermal(const Paths &p = default_paths(),
                                     const ThermalThresholds &th = {})
    {
        ThermalState st;
#if defined(__linux__) || defined(__ANDROID__)
        std::vector<std::string> zones;
        if (!detail::list_dir(p.thermal, zones)) return st;

        for (const std::string &z : zones)
        {
            if (z.rfind("thermal_zone", 0) != 0) continue;
            ++st.zones_seen;
            const std::string base = p.thermal + "/" + z;
            std::string type;
            detail::read_text_file(base + "/type", type);
            if (!zone_is_soc(type)) continue;

            std::string raw;
            if (!detail::read_text_file(base + "/temp", raw))
            {
                ++st.zones_failed;
                continue;
            }
            long long mc = 0;
            if (!detail::parse_int(raw, mc))
            {
                ++st.zones_failed;
                continue;
            }
            // 量级校正：|v| < 1000 时几乎必然是整摄氏度（45000 才是 45°C）。
            // 判据是"值小于 1000"而不是"除以 1000 之后合理"，因为后者会把
            // 真正的 5°C 环境温度(5000)也改掉 —— 我们要的是**不猜**，
            // 这里只做一个可判定的单位归一。
            if (mc > -1000 && mc < 1000) mc *= 1000;

            ++st.zones_used;
            if (mc > st.worst_temp_mc)
            {
                st.worst_temp_mc = (int)mc;
                st.worst_zone = type;
            }
        }
        st.available = st.zones_used > 0;
        st.tier = st.available ? tier_of_temp(st.worst_temp_mc, th) : Tier::Unknown;
#else
        (void)p;
        (void)th;
#endif
        return st;
    }

    // ── 大小核拓扑 ───────────────────────────────────────────────────

    struct Core
    {
        int id = 0;
        long capacity = 0;   ///< cpu_capacity 优先，取不到则用 cpufreq max_freq
        bool from_freq = false;
        int cluster = 0;     ///< 0 = 最小（little），越大越快
    };

    struct Topology
    {
        bool available = false;
        int cores = 0;
        int clusters = 0;
        std::vector<Core> list;
        long capacity_min = -1;
        long capacity_max = -1;
        std::string source;   ///< "capacity" | "freq" | ""（诊断用）
    };

    /// 读 CPU 拓扑。
    ///
    /// 主判据是 `cpuN/cpu_capacity`（内核 sched 的算力值，big.LITTLE 上差异明显）；
    /// 取不到时退回 `cpufreq/cpuinfo_max_freq`（频率不等价于算力，但**顺序**通常
    /// 一致，而且它是唯一普遍存在的）。两者都取不到 → 不可用，不编造。
    inline Topology read_topology(const Paths &p = default_paths())
    {
        Topology t;
#if defined(__linux__) || defined(__ANDROID__)
        std::vector<std::string> names;
        if (!detail::list_dir(p.cpu, names)) return t;

        std::vector<std::pair<int, long>> caps;
        std::string src;
        for (const std::string &n : names)
        {
            if (n.rfind("cpu", 0) != 0 || n.size() < 4) continue;
            const std::string num = n.substr(3);
            long long id = -1;
            if (!detail::parse_int(num, id) || id < 0) continue;

            long long cap = 0;
            std::string v;
            if (detail::read_text_file(p.cpu + "/" + n + "/cpu_capacity", v) &&
                detail::parse_int(v, cap) && cap > 0)
            {
                caps.emplace_back((int)id, (long)cap);
                src = "capacity";
                continue;
            }
            if (detail::read_text_file(p.cpu + "/" + n + "/cpufreq/cpuinfo_max_freq", v) &&
                detail::parse_int(v, cap) && cap > 0)
            {
                caps.emplace_back((int)id, (long)cap);
                if (src.empty()) src = "freq";
            }
        }
        if (caps.empty()) return t;

        std::sort(caps.begin(), caps.end());
        for (size_t i = 1; i < caps.size(); ++i)
            if (caps[i].first == caps[i - 1].first) { caps.erase(caps.begin() + (long)i); --i; }

        // 分簇：按算力排序，相邻比值超过 15% 就新增一簇。用比值而不是"是否相等"，
        // 因为同一簇里各核的 capacity 会有几个百分点的抖动（热降频、随机化），
        // 判等会把一簇切成七八个假簇。
        int cluster = 0;
        long prev = caps.front().second;
        for (size_t i = 0; i < caps.size(); ++i)
        {
            const long c = caps[i].second;
            if (i > 0 && (double)c > (double)prev * 1.15) ++cluster;
            Core core;
            core.id = caps[i].first;
            core.capacity = c;
            core.cluster = cluster;
            core.from_freq = (src == "freq");
            t.list.push_back(core);
            prev = c;
        }
        t.cores = (int)t.list.size();
        t.clusters = cluster + 1;
        t.capacity_min = t.list.front().capacity;
        t.capacity_max = t.list.back().capacity;
        t.source = src;
        t.available = true;
#else
        (void)p;
#endif
        return t;
    }

    /// 性能核（最高簇）的 id 列表。
    inline std::vector<int> performance_cores(const Topology &t)
    {
        std::vector<int> out;
        if (!t.available) return out;
        for (const Core &c : t.list)
            if (c.cluster == t.clusters - 1) out.push_back(c.id);
        return out;
    }

    /// 把 id 列表压成 `sched_setaffinity` 用的范围串（"0-3,7"）。
    /// 压成范围不是为了省字节，是为了**可读**：日志里 `0-3,7` 一眼能看出是
    /// "前四个小核 + 第七个大核"，而 `0,1,2,3,7` 需要数。
    inline std::string compress_cpu_list(const std::vector<int> &ids)
    {
        if (ids.empty()) return "";
        std::vector<int> v = ids;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        std::string out;
        size_t i = 0;
        while (i < v.size())
        {
            size_t j = i;
            while (j + 1 < v.size() && v[j + 1] == v[j] + 1) ++j;
            if (!out.empty()) out.push_back(',');
            out += std::to_string(v[i]);
            if (j > i) { out.push_back('-'); out += std::to_string(v[j]); }
            i = j + 1;
        }
        return out;
    }

    /// 把当前线程钉到性能核上。**由宿主显式调用** —— 库不去偷偷改调度策略。
    /// 返回 false 时 `errno_msg` 说明原因（通常是 EPERM / 平台不支持）。
    inline bool pin_current_thread_to(const std::vector<int> &cpu_ids, std::string *err = nullptr)
    {
#if defined(__linux__) || defined(__ANDROID__)
        if (cpu_ids.empty())
        {
            if (err) *err = "空 CPU 列表";
            return false;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int id : cpu_ids)
        {
            if (id < 0 || id >= CPU_SETSIZE)
            {
                if (err) *err = "CPU id 越界: " + std::to_string(id);
                return false;
            }
            CPU_SET(id, &set);
        }
        if (::sched_setaffinity(0, sizeof(set), &set) != 0)
        {
            if (err) *err = std::string("sched_setaffinity 失败（") + std::strerror(errno) + "）";
            return false;
        }
        return true;
#else
        (void)cpu_ids;
        if (err) *err = "该平台没有 sched_setaffinity";
        return false;
#endif
    }

    // ── TBR / 附件策略（不 include Vulkan，只输出数值）─────────────────

    /// Vulkan 枚举值（写死成 int：本文件不能依赖 Vulkan 头，理由见文件头）。
    namespace vkval
    {
        constexpr int LOAD_OP_LOAD = 0;
        constexpr int LOAD_OP_CLEAR = 1;
        constexpr int LOAD_OP_DONT_CARE = 2;
        constexpr int STORE_OP_STORE = 0;
        constexpr int STORE_OP_DONT_CARE = 1;
        constexpr int IMAGE_LAYOUT_UNDEFINED = 0;
        constexpr int IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2;
        constexpr int IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3;
        constexpr int IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5;
    } // namespace vkval

    enum class GpuKind
    {
        Unknown = 0,
        TileBased,   ///< Mali / Adreno / PowerVR —— TBR/IMR 混合
        Immediate,   ///< 桌面独显/核显
    };

    /// 从渲染器字符串猜 GPU 类别。
    ///
    /// **这是启发式**（标在返回值里，见 `GpuInfo::confident`）：真正的 TBR 判定
    /// 需要 `VK_KHR_...` 扩展或厂商文档，而从字符串猜是最接近的可判定手段。
    /// 猜错只是策略次优（多一次无关的 LOAD_OP_CLEAR），不会画错 —— 所以这里
    /// 允许猜，但把"我在猜"这件事**说出来**。
    inline GpuKind classify_gpu(const std::string &renderer, unsigned vendor_id = 0)
    {
        std::string r;
        r.resize(renderer.size());
        for (size_t i = 0; i < renderer.size(); ++i) r[i] = (char)std::tolower((unsigned char)renderer[i]);
        const char *tbr[] = {"mali", "adreno", "powervr", "videocore", "immortalis", "xclipse"};
        for (const char *k : tbr)
            if (r.find(k) != std::string::npos) return GpuKind::TileBased;
        const char *imr[] = {"nvidia", "geforce", "radeon", "amd", "intel", "apple", "arc"};
        for (const char *k : imr)
            if (r.find(k) != std::string::npos) return GpuKind::Immediate;
        // 厂商 id 兜底：0x13B5=ARM, 0x5143=Qualcomm, 0x1010=Imagination
        if (vendor_id == 0x13B5 || vendor_id == 0x5143 || vendor_id == 0x1010) return GpuKind::TileBased;
        if (vendor_id == 0x10DE || vendor_id == 0x1002 || vendor_id == 0x8086) return GpuKind::Immediate;
        return GpuKind::Unknown;
    }

    struct GpuInfo
    {
        GpuKind kind = GpuKind::Unknown;
        bool confident = false;   ///< 是否来自宿主上报（而不是字符串启发式）
        std::string renderer;
    };

    struct AttachmentPolicy
    {
        int color_load = vkval::LOAD_OP_CLEAR;
        int color_store = vkval::STORE_OP_STORE;
        int depth_load = vkval::LOAD_OP_CLEAR;
        int depth_store = vkval::STORE_OP_DONT_CARE;
        int final_layout = vkval::IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bool transient_depth = false;      ///< LAZILY_ALLOCATED（移动 GPU 上是真省带宽）
        bool prefer_load_op_clear = true;  ///< 用 LOAD_OP_CLEAR 而不是 vkCmdClearAttachments
        bool memoryless_buffers = false;   ///< 颜色附件也可以 memoryless（无需回读时）
    };

    /// 附件策略。
    ///   is_tbr        —— 移动 GPU
    ///   needs_readback—— 本帧的颜色附件之后要被采样（fox 的玻璃模糊就是这种情况）
    ///   has_depth     —— 管线是否用深度
    inline AttachmentPolicy attachment_policy(GpuKind is_tbr, bool needs_readback = false,
                                              bool has_depth = true)
    {
        AttachmentPolicy p;
        const bool tbr = is_tbr == GpuKind::TileBased;
        // 颜色附件：TBR 上 LOAD_OP_CLEAR 是在 tile 内存里清，比单独一次
        // vkCmdClearAttachments 便宜（后者要写一遍全屏带宽）。
        p.color_load = vkval::LOAD_OP_CLEAR;
        p.color_store = needs_readback ? vkval::STORE_OP_STORE : vkval::STORE_OP_DONT_CARE;
        p.prefer_load_op_clear = true;
        p.final_layout = needs_readback ? vkval::IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : vkval::IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (has_depth)
        {
            p.depth_load = vkval::LOAD_OP_CLEAR;
            p.depth_store = vkval::STORE_OP_DONT_CARE;   // fox 不用深度做后处理采样
            p.transient_depth = tbr;
        }
        p.memoryless_buffers = tbr && !needs_readback;
        return p;
    }

    // ── 宿主上报（A2a 不猜这些）───────────────────────────────────────

    /// A2a 的全局状态。宿主把**只有它知道**的事实报告进来：
    ///   · 显示旋转（来自 currentTransform / Display.getRotation，需要 JNI 上下文）
    ///   · 是否支持 VK_GOOGLE_display_timing（来自扩展枚举）
    ///   · 真实的 GPU renderer 字符串（来自 VkPhysicalDeviceProperties）
    struct Reports
    {
        int display_rotation = -1;      ///< 0/90/180/270；-1 = 宿主没报过
        bool display_timing = false;
        bool display_timing_known = false;
        GpuInfo gpu;
    };

    inline Reports &reports()
    {
        static Reports r;
        return r;
    }

    inline void report_display_rotation(int degrees)
    {
        reports().display_rotation = (degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270)
                                         ? degrees
                                         : -1;
    }

    inline void report_display_timing(bool supported)
    {
        reports().display_timing = supported;
        reports().display_timing_known = true;
    }

    inline void report_gpu(const std::string &renderer, unsigned vendor_id = 0)
    {
        Reports &r = reports();
        r.gpu.renderer = renderer;
        r.gpu.kind = classify_gpu(renderer, vendor_id);
        r.gpu.confident = true;
    }

    /// 合成器是否已经在替我们旋转（即我们不需要自己转）。
    ///
    /// Android 上 `ANativeWindow` 的 buffer 尺寸在旋转时不变，合成器负责转 ——
    /// 于是**应用自己去转会把画面转两次**（这个 bug 在真机上表现为"横屏时画面
    /// 倒着"）。可判定的是"宿主报了多少度"；-1（没报过）时返回 false，并让调用方
    /// 自己决定 —— 不知道就不要动。
    inline bool compositor_handles_rotation()
    {
        const int deg = reports().display_rotation;
        return deg == 0 || deg == 90 || deg == 180 || deg == 270;
    }

    // ── 质量档（降级顺序是设计的一部分）──────────────────────────────

    enum class Blur : int
    {
        Full = 0,
        Half = 1,   ///< 半分辨率降采样 + 一次模糊
        Off = 2,
    };

    enum class Shadows : int
    {
        Full = 0,
        Reduced = 1,  ///< 阴影半径减半、去掉第二层
        Off = 2,
    };

    struct Quality
    {
        Tier tier = Tier::Unknown;
        Blur blur = Blur::Full;
        Shadows shadows = Shadows::Full;
        bool animations = true;
        int target_fps = 60;    ///< **最后一个**被降的
        bool vrs_allowed = true; ///< 允许后端接 Pa 的 rate map（见 docs/gfx-acceleration.md §1.2）
    };

    inline Quality quality_for(Tier t)
    {
        Quality q;
        q.tier = t;
        switch (t)
        {
        case Tier::Normal:
        case Tier::Unknown:   // 读不到热状态：按最保守的"没问题"处理，而不是按最差
            break;
        case Tier::Light:
            q.blur = Blur::Half;
            break;
        case Tier::Moderate:
            q.blur = Blur::Half;
            q.shadows = Shadows::Reduced;
            break;
        case Tier::Severe:
            q.blur = Blur::Off;
            q.shadows = Shadows::Reduced;
            q.animations = false;
            break;
        case Tier::Critical:
            q.blur = Blur::Off;
            q.shadows = Shadows::Off;
            q.animations = false;
            q.target_fps = 45;
            q.vrs_allowed = false;
            break;
        case Tier::Emergency:
            q.blur = Blur::Off;
            q.shadows = Shadows::Off;
            q.animations = false;
            q.target_fps = 30;
            q.vrs_allowed = false;
            break;
        case Tier::Shutdown:
            q.blur = Blur::Off;
            q.shadows = Shadows::Off;
            q.animations = false;
            q.target_fps = 15;
            q.vrs_allowed = false;
            break;
        }
        return q;
    }

    // ── 惰性入口 ─────────────────────────────────────────────────────

    /// Android 上且 sysfs 真的能读 → true。其它平台**一律 false**。
    ///
    /// 这是纯运行期判定（不是 `#if` 常量）：Android 上容器/受限环境里
    /// `/sys/class/thermal` 也可能不可读，那时"可用"就是 false ——
    /// 宿主据此走桌面策略，而不是拿一堆 -1 硬跑。
    inline bool available()
    {
        if (!is_android_build) return false;
        const ThermalState t = read_thermal();
        const Topology tp = read_topology();
        return t.available || tp.available;
    }

    /// 一次性决策：本帧用哪个质量档、附件怎么配。非 Android 上返回中性值
    /// （Full/60fps），因为"没有证据"不等于"设备很热"。
    inline Quality current_quality()
    {
        if (!is_android_build) return quality_for(Tier::Unknown);
        const ThermalState t = read_thermal();
        return quality_for(t.tier);
    }
}