#include "utils/A2a.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace a2a;

namespace
{
    int g_fail = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) ++g_fail;
    }

    std::string g_root = "a2a_fixtures";

    void put(const fs::path &p, const std::string &v)
    {
        fs::create_directories(p.parent_path());
        std::ofstream o(p, std::ios::binary | std::ios::trunc);
        o << v << "\n";
    }

    void make_thermal(const std::string &name,
                      const std::vector<std::pair<std::string, std::string>> &zones)
    {
        const fs::path base = fs::path(g_root) / name;
        fs::remove_all(base);
        for (size_t i = 0; i < zones.size(); ++i)
        {
            const fs::path z = base / ("thermal_zone" + std::to_string(i));
            put(z / "type", zones[i].first);
            if (!zones[i].second.empty()) put(z / "temp", zones[i].second);
        }
    }

    void make_cpu_capacity(const std::string &name, const std::vector<long> &caps)
    {
        const fs::path base = fs::path(g_root) / name;
        fs::remove_all(base);
        for (size_t i = 0; i < caps.size(); ++i)
            put(base / ("cpu" + std::to_string(i)) / "cpu_capacity", std::to_string(caps[i]));
    }

    void make_cpu_freq(const std::string &name, const std::vector<long> &khz)
    {
        const fs::path base = fs::path(g_root) / name;
        fs::remove_all(base);
        for (size_t i = 0; i < khz.size(); ++i)
            put(base / ("cpu" + std::to_string(i)) / "cpufreq" / "cpuinfo_max_freq",
                std::to_string(khz[i]));
    }

    Paths paths_for(const std::string &thermal, const std::string &cpu)
    {
        Paths p;
        p.thermal = (fs::path(g_root) / thermal).string();
        p.cpu = (fs::path(g_root) / cpu).string();
        p.cpuinfo = (fs::path(g_root) / "cpuinfo").string();
        return p;
    }
}

int main()
{
    if (const char *e = std::getenv("A2A_FIXTURE_ROOT")) g_root = e;
    fs::create_directories(g_root);

    std::printf("[1] 非 Android 上必须惰性（本机 is_android_build=%d）\n", (int)is_android_build);
    check(!is_android_build || true, "编译期平台判定存在");
    if (!is_android_build)
    {
        check(!available(), "available() == false");
        const Quality q = current_quality();
        check(q.blur == Blur::Full && q.shadows == Shadows::Full && q.animations &&
                  q.target_fps == 60 && q.tier == Tier::Unknown,
              "current_quality() 是中性值（没有证据 ≠ 设备很热）");
        check(reports().display_rotation == -1, "显示旋转未上报时是 -1（可判定，不是 0）");
        check(!compositor_handles_rotation(), "没上报过旋转时不声称合成器会替我们转");
        std::string err;
        check(!pin_current_thread_to({}, &err) && !err.empty(), "空 CPU 列表 + 可读的失败原因");
    }

    std::printf("[2] thermal：zone 过滤 / 单位归一 / 读失败记账\n");
    {
        make_thermal("t_battery_hot",
                     {{"battery", "88000"}, {"charger", "90000"}, {"cpu-0-0", "45000"},
                      {"gpu", "52000"}});
        ThermalState st = read_thermal(paths_for("t_battery_hot", "c_none"));
        check(st.available, "读到 SoC zone");
        check(st.zones_seen == 4, "zone 总数记账（含被过滤的）");
        check(st.zones_used == 2, "只有 2 个 SoC zone 参与判定");
        check(st.worst_temp_mc == 52000 && st.worst_zone == "gpu", "最热 SoC zone = gpu 52°C");
        check(st.tier == Tier::Normal, "等级 = normal（电池的 88°C 不参与）");

        make_thermal("t_hot", {{"cpu-0-0", "82000"}, {"gpu", "78000"}});
        st = read_thermal(paths_for("t_hot", "c_none"));
        check(st.tier == Tier::Severe, "82°C → severe（阈值 80°C）");

        make_thermal("t_emerg", {{"tsens_tz_sensor0", "91000"}});
        check(read_thermal(paths_for("t_emerg", "c_none")).tier == Tier::Emergency,
              "91°C → emergency");

        make_thermal("t_celsius", {{"cpu-0-0", "65"}});
        ThermalState c = read_thermal(paths_for("t_celsius", "c_none"));
        check(c.worst_temp_mc == 65000 && c.tier == Tier::Light,
              "整摄氏度被归一（65 → 65000 → light），而不是\"永远不热\"");

        make_thermal("t_missing", {{"cpu-0-0", ""}, {"cpu-1-0", "50000"}});
        st = read_thermal(paths_for("t_missing", "c_none"));
        check(st.zones_failed == 1 && st.zones_used == 1, "读失败的 zone 单独记账");
        check(st.worst_temp_mc == 50000, "失败的那个不影响最热判定");

        st = read_thermal(paths_for("t_nope", "c_none"));
        check(!st.available && st.tier == Tier::Unknown && st.worst_temp_mc == -1,
              "目录不存在 → available=false 且值为 -1（可判定）");

        make_thermal("t_only_battery", {{"battery", "40000"}});
        check(!read_thermal(paths_for("t_only_battery", "c_none")).available,
              "只有电池 zone → 不可用（不冒充\"设备很凉\"）");
    }

    std::printf("[3] 拓扑：分簇 / 抖动 / 三簇 / freq 兜底\n");
    {
        make_cpu_capacity("c_8", {512, 512, 512, 512, 1024, 1024, 1024, 1024});
        Topology t = read_topology(paths_for("t_none", "c_8"));
        check(t.available && t.cores == 8 && t.clusters == 2, "8 核 2 簇");
        check(t.source == "capacity", "优先用 cpu_capacity");
        check(performance_cores(t) == std::vector<int>({4, 5, 6, 7}), "性能核 = 4..7");
        check(compress_cpu_list(performance_cores(t)) == "4-7", "范围压缩 → \"4-7\"");

        make_cpu_capacity("c_jitter", {500, 510, 520, 1000});
        t = read_topology(paths_for("t_none", "c_jitter"));
        check(t.clusters == 2 && t.cores == 4, "抖动（500/510/520）仍是一簇");

        make_cpu_capacity("c_3", {400, 400, 600, 600, 1000, 1000});
        t = read_topology(paths_for("t_none", "c_3"));
        check(t.clusters == 3, "400/600/1000 → 三簇");
        check(performance_cores(t) == std::vector<int>({4, 5}), "性能核取最高簇");
        check(compress_cpu_list({0, 1, 2, 3, 7}) == "0-3,7", "压缩混合范围");

        make_cpu_freq("c_freq", {1200000, 1200000, 2400000, 2400000});
        t = read_topology(paths_for("t_none", "c_freq"));
        check(t.available && t.clusters == 2 && t.source == "freq", "freq 兜底路径可用");
        check(t.list.front().from_freq && !t.list.back().from_freq == false, "核标记了来源");

        t = read_topology(paths_for("t_none", "c_empty"));
        check(!t.available && t.cores == 0 && performance_cores(t).empty(),
              "没有任何算力信息 → 不可用（不编造核数）");
    }

    std::printf("[4] TBR 判定与附件策略（不猜就是不说）\n");
    {
        check(classify_gpu("Mali-G78 MP14") == GpuKind::TileBased, "Mali → TBR");
        check(classify_gpu("Adreno (TM) 650") == GpuKind::TileBased, "Adreno → TBR");
        check(classify_gpu("PowerVR Rogue GE8320") == GpuKind::TileBased, "PowerVR → TBR");
        check(classify_gpu("NVIDIA GeForce RTX 4070") == GpuKind::Immediate, "NVIDIA → 非 TBR");
        check(classify_gpu("AMD Radeon RX 7900") == GpuKind::Immediate, "AMD → 非 TBR");
        check(classify_gpu("", 0x13B5) == GpuKind::TileBased, "空字符串靠厂商 id（ARM）");
        check(classify_gpu("llvmpipe") == GpuKind::Unknown, "不认识就说不知道");

        AttachmentPolicy p = attachment_policy(GpuKind::TileBased, false, true);
        check(p.color_load == vkval::LOAD_OP_CLEAR && p.color_store == vkval::STORE_OP_DONT_CARE,
              "TBR + 不回读：颜色 LOAD=CLEAR / STORE=DONT_CARE");
        check(p.depth_load == vkval::LOAD_OP_CLEAR && p.depth_store == vkval::STORE_OP_DONT_CARE &&
                  p.transient_depth && p.memoryless_buffers,
              "TBR：深度 STORE=DONT_CARE + transient + memoryless 颜色");
        check(p.prefer_load_op_clear, "用 LOAD_OP_CLEAR 而不是 vkCmdClearAttachments");
        check(p.final_layout == vkval::IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              "不回读 → 不必转 SHADER_READ_ONLY");

        p = attachment_policy(GpuKind::TileBased, true, true);
        check(p.color_store == vkval::STORE_OP_STORE && !p.memoryless_buffers,
              "要回读：颜色必须 STORE 且不能 memoryless（否则玻璃采样到垃圾）");
        check(p.final_layout == vkval::IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              "要回读 → 转 SHADER_READ_ONLY");

        p = attachment_policy(GpuKind::Immediate, false, true);
        check(!p.transient_depth && !p.memoryless_buffers, "桌面 GPU：不加移动端专属状态");

        report_gpu("Mali-G78 MP14", 0x13B5);
        check(reports().gpu.confident && reports().gpu.kind == GpuKind::TileBased,
              "宿主上报的 GPU 信息标记为可信（区别于字符串启发式）");
    }

    std::printf("[5] 降级顺序：帧率必须是最后一个被降的\n");
    {
        const Tier tiers[] = {Tier::Unknown, Tier::Normal, Tier::Light, Tier::Moderate,
                              Tier::Severe,  Tier::Critical, Tier::Emergency, Tier::Shutdown};
        bool mono = true, fps_last = true, unknown_plural = true;
        int prev_blur = -1, prev_sh = -1, prev_fps = 1 << 20;
        bool prev_anim = true;
        for (Tier t : tiers)
        {
            const Quality q = quality_for(t);
            const int blur = (int)q.blur, sh = (int)q.shadows;
            if (blur < prev_blur || sh < prev_sh) mono = false;
            if (!prev_anim && q.animations) mono = false;
            if (q.target_fps > prev_fps) mono = false;
            if (q.target_fps < 60 && (q.blur != Blur::Off || q.shadows != Shadows::Off))
                fps_last = false;
            prev_blur = blur;
            prev_sh = sh;
            prev_anim = q.animations;
            prev_fps = q.target_fps;
        }
        check(mono, "档位升高时能力单调不增");
        check(fps_last, "帧率下降 ⟹ 模糊/阴影已经全关（帧率最后才降）");

        const Quality norm = quality_for(Tier::Normal);
        const Quality unk = quality_for(Tier::Unknown);
        unknown_plural = norm.blur == unk.blur && norm.shadows == unk.shadows &&
                         norm.target_fps == unk.target_fps && norm.animations == unk.animations;
        check(unknown_plural, "读不到热状态 → 按最保守的\"没问题\"处理，而不是按最差");

        const Quality crit = quality_for(Tier::Critical);
        check(crit.blur == Blur::Off && crit.shadows == Shadows::Off && crit.target_fps == 45,
              "critical：模糊/阴影全关后才把帧率降到 45");
        check(!crit.vrs_allowed, "critical 时不再接 Pa 的 rate map（VRS 也是质量）");
    }

    std::printf("[6] 宿主上报：不猜需要上下文的东西\n");
    {
        report_display_rotation(90);
        check(reports().display_rotation == 90 && compositor_handles_rotation(),
              "上报 90° → 合成器负责旋转");
        report_display_rotation(45);
        check(reports().display_rotation == -1 && !compositor_handles_rotation(),
              "非法角度（45°）被拒绝并回到 -1，而不是被当成有效值");
        report_display_rotation(0);
        check(reports().display_rotation == 0 && compositor_handles_rotation(), "0° 是合法上报");
        report_display_timing(true);
        check(reports().display_timing && reports().display_timing_known,
              "display timing 支持与否由宿主上报（A2a 不枚举扩展）");
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "A2A: 全部通过" : "A2A: 失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}