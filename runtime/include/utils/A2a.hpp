#pragma once

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

#if defined(__ANDROID__)
    constexpr bool is_android_build = true;
#else
    constexpr bool is_android_build = false;
#endif

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
        if (temp_mc <= 0) return Tier::Unknown;
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
        bool available = false;
        Tier tier = Tier::Unknown;
        int worst_temp_mc = -1;
        int zones_seen = 0;
        int zones_used = 0;
        int zones_failed = 0;
        std::string worst_zone;
    };

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
    }

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


    struct Core
    {
        int id = 0;
        long capacity = 0;
        bool from_freq = false;
        int cluster = 0;
    };

    struct Topology
    {
        bool available = false;
        int cores = 0;
        int clusters = 0;
        std::vector<Core> list;
        long capacity_min = -1;
        long capacity_max = -1;
        std::string source;
    };

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

    inline std::vector<int> performance_cores(const Topology &t)
    {
        std::vector<int> out;
        if (!t.available) return out;
        for (const Core &c : t.list)
            if (c.cluster == t.clusters - 1) out.push_back(c.id);
        return out;
    }

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
    }

    enum class GpuKind
    {
        Unknown = 0,
        TileBased,
        Immediate,
    };

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
        if (vendor_id == 0x13B5 || vendor_id == 0x5143 || vendor_id == 0x1010) return GpuKind::TileBased;
        if (vendor_id == 0x10DE || vendor_id == 0x1002 || vendor_id == 0x8086) return GpuKind::Immediate;
        return GpuKind::Unknown;
    }

    struct GpuInfo
    {
        GpuKind kind = GpuKind::Unknown;
        bool confident = false;
        std::string renderer;
    };

    struct AttachmentPolicy
    {
        int color_load = vkval::LOAD_OP_CLEAR;
        int color_store = vkval::STORE_OP_STORE;
        int depth_load = vkval::LOAD_OP_CLEAR;
        int depth_store = vkval::STORE_OP_DONT_CARE;
        int final_layout = vkval::IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bool transient_depth = false;
        bool prefer_load_op_clear = true;
        bool memoryless_buffers = false;
    };

    inline AttachmentPolicy attachment_policy(GpuKind is_tbr, bool needs_readback = false,
                                              bool has_depth = true)
    {
        AttachmentPolicy p;
        const bool tbr = is_tbr == GpuKind::TileBased;
        p.color_load = vkval::LOAD_OP_CLEAR;
        p.color_store = needs_readback ? vkval::STORE_OP_STORE : vkval::STORE_OP_DONT_CARE;
        p.prefer_load_op_clear = true;
        p.final_layout = needs_readback ? vkval::IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : vkval::IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (has_depth)
        {
            p.depth_load = vkval::LOAD_OP_CLEAR;
            p.depth_store = vkval::STORE_OP_DONT_CARE;
            p.transient_depth = tbr;
        }
        p.memoryless_buffers = tbr && !needs_readback;
        return p;
    }


    struct Reports
    {
        int display_rotation = -1;
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

    inline bool compositor_handles_rotation()
    {
        const int deg = reports().display_rotation;
        return deg == 0 || deg == 90 || deg == 180 || deg == 270;
    }


    enum class Blur : int
    {
        Full = 0,
        Half = 1,
        Off = 2,
    };

    enum class Shadows : int
    {
        Full = 0,
        Reduced = 1,
        Off = 2,
    };

    struct Quality
    {
        Tier tier = Tier::Unknown;
        Blur blur = Blur::Full;
        Shadows shadows = Shadows::Full;
        bool animations = true;
        int target_fps = 60;
        bool vrs_allowed = true;
    };

    inline Quality quality_for(Tier t)
    {
        Quality q;
        q.tier = t;
        switch (t)
        {
        case Tier::Normal:
        case Tier::Unknown:
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


    inline bool available()
    {
        if (!is_android_build) return false;
        const ThermalState t = read_thermal();
        const Topology tp = read_topology();
        return t.available || tp.available;
    }

    inline Quality current_quality()
    {
        if (!is_android_build) return quality_for(Tier::Unknown);
        const ThermalState t = read_thermal();
        return quality_for(t.tier);
    }
}