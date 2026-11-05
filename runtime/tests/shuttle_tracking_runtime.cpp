// ============================================================================
// shuttle_tracking_runtime.cpp —— 「穿梭」/「追踪」运行期执行体的一致性校验
//
// 直接对 **编译器真实产出的调度表** 运行 include/utils/ShuttleRuntime.hpp，
// 而不是对一份手写 JSON 运行。手写 JSON 会掩盖最危险的一类缺陷：
// 编译器序列化格式变了、运行期解析没跟上（或反之）——此时双方各自「自洽」，
// 但接起来就是错的，且不会有任何报错。
//
// 内嵌的 WIRE_JSON 常量来自 server/src/topology.ts + ir.ts 的真实输出
// （程序：sense@(0,0,t0) shuttle→(1,0) track=(expect_z,entropy)，
//         relay@(1,0,t1) track=(expect_z)，sink@(1,1,t2)）。
//
// 编译运行：
//   clang++ -std=c++20 -O0 -I runtime/include \
//           -o /tmp/shuttle_rt runtime/tests/shuttle_tracking_runtime.cpp && /tmp/shuttle_rt
// ============================================================================

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "utils/ShuttleRuntime.hpp"
#include "qhal/json.hpp"

// ── 编译器真实产出（由 server 端生成后原样内嵌）─────────────────────────────
static const char *WIRE_JSON = R"JSON(
{"shape":{"time":3,"thread":1,"coord":[2,2]},
 "blocks":[{"name":"sense","time":0,"thread":0,"coord":[0,0],"cost":1,"deadline":null},
           {"name":"relay","time":1,"thread":0,"coord":[1,0],"cost":1,"deadline":null},
           {"name":"sink","time":2,"thread":0,"coord":[1,1],"cost":1,"deadline":null}],
 "callGraph":[],
 "shuttleLinks":[{"from":"sense",
   "targets":[{"name":"relay","coord":[1,0],"logWeight":-1,"weight":0.36787944117144233,
               "distance":1,"via":"parallel"}],
   "lambda":1,"phase":2,"rounds":2,"strategy":-1,"slotsPerPhase":[2,2]}],
 "trackingScopes":[{"block":"sense","observables":["expect_z","entropy"],"mode":0,"slots":2,
                    "level":[0,1],"window":[0,1]},
                   {"block":"relay","observables":["expect_z"],"mode":0,"slots":1,
                    "level":[0],"window":[1,2]}]}
)JSON";

// ── 旧调度表（无 shuttleLinks / trackingScopes）→ 必须完全惰性 ──────────────
static const char *LEGACY_JSON = R"JSON(
{"shape":{"time":2,"thread":1,"coord":[1]},
 "blocks":[{"name":"a","time":0,"thread":0,"coord":[0],"cost":1,"deadline":null},
           {"name":"b","time":1,"thread":0,"coord":[0],"cost":1,"deadline":null}],
 "callGraph":[]}
)JSON";

// ── 极远对端：d/λ = 999 → 线性权重下溢，但必须仍被调度 ──────────────────────
static const char *UNDERFLOW_JSON = R"JSON(
{"shape":{"time":2,"thread":1,"coord":[1000,2]},
 "blocks":[{"name":"near","time":0,"thread":0,"coord":[0,0],"cost":1,"deadline":null},
           {"name":"far","time":1,"thread":0,"coord":[999,0],"cost":1,"deadline":null}],
 "shuttleLinks":[{"from":"near",
   "targets":[{"name":"far","coord":[999,0],"logWeight":-999,"weight":0,
               "distance":999,"via":"parallel"}],
   "lambda":1,"phase":2,"rounds":1,"strategy":-1,"slotsPerPhase":[1,1]}],
 "trackingScopes":[{"block":"near","observables":["expect_z"],"mode":0,"slots":1,
                    "level":[0],"window":[0,1]}]}
)JSON";

// ── 强制 qnd（mode=1）作用于非 Pauli 可观测量 "entropy"：
//    编译期 E-TRK003 会拦截；此处手造仿佛数据验证运行期最后一道防线。──────────
static const char *FORCED_QND_JSON = R"JSON(
{"shape":{"time":1,"thread":1,"coord":[1]},
 "blocks":[{"name":"bad","time":0,"thread":0,"coord":[0],"cost":1,"deadline":null}],
 "trackingScopes":[{"block":"bad","observables":["entropy"],"mode":1,"slots":1,
                    "level":[0],"window":[0,1]}]}
)JSON";

// ── c001 蝶点：strategy=4，全体层主要特征压缩成点再辐射 ────────────────────
// 编译期解析深度恒 1（与 span / 对端数无关），运行期在 capture 时显式物化
// 蝶点摘要（stats.c001_points + 诊断日志），deliver 时按相位向全体辐射。
static const char *C001_JSON = R"JSON(
{"shape":{"time":2,"thread":1,"coord":[64,2]},
 "blocks":[{"name":"hub","time":0,"thread":0,"coord":[0,0],"cost":1,"deadline":null},
           {"name":"leaf_a","time":1,"thread":0,"coord":[32,0],"cost":1,"deadline":null},
           {"name":"leaf_b","time":1,"thread":0,"coord":[64,0],"cost":1,"deadline":null}],
 "shuttleLinks":[{"from":"hub",
   "targets":[{"name":"leaf_a","coord":[32,0],"logWeight":-32,"weight":1.2664165549094176e-14,
               "distance":32,"via":"parallel"},
              {"name":"leaf_b","coord":[64,0],"logWeight":-64,"weight":1.603810890548638e-28,
               "distance":64,"via":"parallel"}],
   "lambda":1,"phase":2,"rounds":1,"strategy":4,"slotsPerPhase":[1,1]}],
 "trackingScopes":[{"block":"hub","observables":["expect_z","entropy"],"mode":0,"slots":2,
                    "level":[0,1],"window":[0,1]},
                   {"block":"leaf_a","observables":["expect_z"],"mode":0,"slots":1,
                    "level":[0],"window":[1,2]},
                   {"block":"leaf_b","observables":["expect_z"],"mode":0,"slots":1,
                    "level":[0],"window":[1,2]}]}
)JSON";

static int g_fail = 0;
static void check(bool cond, const char *what)
{
    std::printf("  %s %s\n", cond ? "ok:  " : "FAIL:", what);
    if (!cond)
        ++g_fail;
}
static bool nearly(double a, double b, double eps = 1e-12)
{
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

int main()
{
    const double w1 = std::exp(-1.0);   // relay 对 sense 的关联度

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[1] 解析编译器真实调度表\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(WIRE_JSON));

        check(rt.count_shuttles() == 1, "解析出 1 条穿梭链接");
        check(rt.count_scopes() == 2, "解析出 2 个追踪作用域");
        check(rt.has_scope("sense") && rt.has_scope("relay"), "sense 与 relay 均有作用域");
        check(!rt.has_scope("sink"), "sink 无作用域（不伪造）");

        const ta::ShuttleLink *lk = rt.link_of("sense");
        check(lk != nullptr, "链接 source = sense");
        check(lk && lk->peers.size() == 1, "1 个关联对端");
        check(lk && lk->phase == ta::ShuttlePhase::Both, "phase=2 → Both（前后都渗透）");
        check(lk && lk->strategy == ta::ShuttleStrategy::Auto, "strategy=-1 → Auto");
        check(lk && lk->rounds == 2, "rounds=2（编译器已解出 auto）");
        check(lk && nearly(lk->peers[0].logWeight, -1.0), "logWeight = -1（权威路由量）");
        check(lk && nearly(lk->peers[0].weight, w1), "weight = exp(-1)");

        const ta::TrackingScopeRt *s = rt.scope_of("sense");
        check(s && s->plans.size() == 2, "sense 有 2 个可观测量");
        check(s && s->plans[0].level == ta::TrackLevel::Qnd, "expect_z → L0/qnd");
        check(s && s->plans[1].level == ta::TrackLevel::Shadow, "entropy → L1/shadow");
        check(s && s->plans[0].extract == ta::ExtractKind::QndPauli, "L0 抽取方式 = QndPauli");
        check(s && s->plans[1].extract == ta::ExtractKind::ShadowSample, "L1 抽取方式 = ShadowSample");
        check(s && s->plans[1].samples > 0, "L1 采样数 > 0（否则无法构成阴影）");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[2] 规划：auto 策略解析 + 决策落盘（不静默）\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(WIRE_JSON));
        rt.plan_all(/*ancilla_bits=*/-1, /*coord_span=*/4);

        const ta::ShuttlePlan *p = rt.plan_of("sense");
        check(p != nullptr, "规划出 sense 的穿梭计划");
        // n=1 对端 ≤ 2 → auto 选蝶式（蝶式在极小规模无劣势）
        check(p && p->strategy == ta::ShuttleStrategy::Butterfly, "auto: N≤2 → 蝶式(0)");
        check(p && p->depth == 2, "深度 = 2 轮（来自 wire 的 rounds）");
        check(!p->degraded, "未发生降级");
        check(rt.stats().w_shu001 == 0, "sense 有 @track → 无 W-SHU001");
        check(sink.str().find("(auto)") != std::string::npos,
              "auto 决策写入诊断（决策不静默）");
        check(sink.str().find("logWeight=-1") != std::string::npos,
              "对端 logWeight 写入诊断");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[3] 追踪：作用域生命周期（影子槽随周期分配与释放）\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(WIRE_JSON));
        rt.plan_all(-1, 4);

        rt.scope_enter("sense");
        std::vector<double> v;
        check(rt.shadow_all("sense", "expect_z", v) && v.size() == 2,
              "expect_z 分配 2 份影子槽（track_slots=2）");
        check(rt.shadow_all("sense", "entropy", v) && v.size() == 2,
              "entropy 分配 2 份影子槽");
        check(!rt.shadow_all("sense", "measure_x", v),
              "未声明的可观测量无影子槽（不凭空造）");

        rt.shadow_write("sense", "expect_z", 0.75);

        rt.scope_exit("sense");
        check(!rt.shadow_all("sense", "expect_z", v),
              "作用域退出后影子槽已释放 → 周期外不可见，不构成持久克隆");
        check(rt.stats().invalid_scopes == 0, "契约满足，作用域有效");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[4] 相位编排：轮次在**目标层** time-slot 边界执行（docs §3.2）\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(WIRE_JSON));
        rt.plan_all(-1, 4);

        // ── 层 0：sense（源）。此时 relay 尚未开影子 → pre 投递无处落地 ──
        rt.layer_enter({"sense"});
        rt.shadow_write("sense", "expect_z", 0.75);
        rt.layer_exit({"sense"});
        check(rt.captured_of("sense"), "源端载荷已捕获（含 expect_z）");
        double pay = 0.0;
        check(rt.payload_of("sense", "expect_z", pay) && nearly(pay, 0.75),
              "载荷 expect_z = 0.75（经典关联标量，非量子态副本）");

        // ── 层 1：relay（目标）。pre 相位把载荷投递进 relay 的影子 ──
        const size_t before = rt.stats().deliveries;
        rt.layer_enter({"relay"});          // 开 relay 影子 + pre 投递
        double rv = 0.0;
        check(rt.shadow_first("relay", "expect_z", rv), "relay 影子已开");
        // 2 轮 × 1 条边 × (0.75 × exp(-1))
        const double expect = 2.0 * 0.75 * w1;
        check(nearly(rv, expect),
              "pre 投递后 relay.expect_z = 2 × 0.75 × exp(-1)（轮次加权累积）");
        check(rt.stats().deliveries == before + 2, "2 轮各投递 1 条边");
        rt.layer_exit({"relay"});
        check(rt.stats().returns == 1, "post 相位产生 1 次返回腿（双侧恢复的另一侧）");
        double back = 0.0;
        check(rt.payload_of("sense", "expect_z", back) && nearly(back, expect),
              "返回腿把 relay 的值回灌进载荷（前后都渗透）");

        // ── 层 2：sink 无作用域、也不是任何链接的目标 → 完全惰性 ──
        const size_t d2 = rt.stats().deliveries;
        rt.layer_enter({"sink"});
        rt.layer_exit({"sink"});
        check(rt.stats().deliveries == d2, "sink 层不产生任何投递");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[5] 退化路径：目标无影子 → 逐轮计数，不静默\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(WIRE_JSON));
        rt.plan_all(-1, 4);

        rt.layer_enter({"sense"});
        rt.shadow_write("sense", "expect_z", 0.5);
        rt.layer_exit({"sense"});   // 捕获载荷

        // 直接对目标投递，但 relay 未开作用域 → 无处落地
        rt.deliver_target("relay", /*pre=*/true);
        check(rt.stats().no_target_shadow == 2, "2 轮均记录「无影对端」（docs §5）");
        check(sink.str().find("退化路径") != std::string::npos,
              "退化路径写入诊断，不静默");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[6] 「哪怕关联度极小也有效」：下溢对端仍被调度\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(UNDERFLOW_JSON));
        rt.plan_all(-1, 1000LL * 2);

        const ta::ShuttlePlan *p = rt.plan_of("near");
        check(p != nullptr, "极远对端仍在计划中");
        check(p && p->rounds.size() == 1 && p->rounds[0].edges.size() == 1,
              "对端仍出现在轮次里（未被丢弃）");
        check(p && p->rounds[0].edges[0].peer == "far", "边指向 far");
        check(p && p->rounds[0].edges[0].underflow,
              "该边被标记为「线性权重下溢」，但保留在计划中");
        check(p && p->rounds[0].edges[0].weight == 0.0,
              "线性权重为数值零（exp(-999) 双精度下溢）");
        check(p && nearly(p->rounds[0].edges[0].logWeight, -999.0),
              "logWeight = -999 仍有限 → 路由不丢对端");
        check(rt.stats().underflow_peers == 1, "下溢对端计入统计（可观测）");
        check(sink.str().find("不丢弃") != std::string::npos,
              "下溢对端写入诊断，与「被阈值截断」明确区分");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[7] 保真阶梯契约（L0/L2 精确；L1 统计级）\n");
    {
        check(ta::TrackingPlanner::fidelity_contract(ta::TrackLevel::Qnd, 1.0),
              "L0 承诺精确（fid=1 通过）");
        check(!ta::TrackingPlanner::fidelity_contract(ta::TrackLevel::Qnd, 0.999),
              "L0 不满足精确即失败（不放过）");
        check(ta::TrackingPlanner::fidelity_contract(ta::TrackLevel::Relabel, 1.0),
              "L2 承诺精确（态从未被触碰）");
        check(ta::TrackingPlanner::fidelity_contract(ta::TrackLevel::Shadow, 0.0),
              "L1 为统计级：任意有限保真度不视为违约");
        const double b21 = ta::TrackingPlanner::shadow_fidelity_bound(21);
        const double b2100 = ta::TrackingPlanner::shadow_fidelity_bound(2100);
        check(b21 > 0.0 && b21 < b2100 && b2100 < 1.0,
              "L1 采样收敛上界随采样数单调趋近 1");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[8] 运行期最后防线：L0 作用于非 Pauli 族 → 作用域失效\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(FORCED_QND_JSON));
        rt.plan_all(-1, 1);

        const ta::TrackingScopeRt *s = rt.scope_of("bad");
        check(s && s->plans[0].level == ta::TrackLevel::Qnd, "强制 mode=1 → 级别被置为 L0");

        rt.scope_enter("bad");
        rt.scope_exit("bad");
        check(rt.stats().invalid_scopes == 1,
              "非 Pauli 观测量不存在非破坏读出电路 → 作用域判定失效");
        check(sink.str().find("INVALID") != std::string::npos,
              "失效写入诊断，不伪造成功");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[9] 旧调度表（无两段）→ 完全惰性，与实际行为兼容\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(LEGACY_JSON));
        rt.plan_all(-1, 1);

        check(rt.count_shuttles() == 0 && rt.count_scopes() == 0, "无穿梭、无追踪");
        rt.layer_enter({"a"});
        rt.layer_exit({"a"});
        rt.layer_enter({"b"});
        rt.layer_exit({"b"});
        check(rt.stats().deliveries == 0 && rt.stats().captures == 0 &&
                  rt.stats().returns == 0 && rt.stats().invalid_scopes == 0,
              "全部计数为 0 → 惰性空操作");
        check(sink.str().empty(), "无任何输出（对既有程序零扰动）");
    }

    // ══════════════════════════════════════════════════════════════════════
    std::printf("[10] c001 蝶点：压缩成点再辐射（strategy=4）\n");
    {
        std::ostringstream sink;
        rvdl::ShuttleRuntime rt(&sink);
        rt.parse(qhal::json::parse(C001_JSON));
        rt.plan_all(-1, 65LL * 2);

        // ① 规划：恒 1 轮、一轮覆盖全体对端（远端对端不随 span 加深）
        const ta::ShuttlePlan *p = rt.plan_of("hub");
        check(p != nullptr, "规划出 hub 的 c001 计划");
        check(p && p->strategy == ta::ShuttleStrategy::C001Point, "strategy=4 → C001Point");
        check(p && p->depth == 1, "深度恒 1 轮（span=130、对端相距 32/64 也不加深）");
        check(p && p->rounds.size() == 1 && p->rounds[0].edges.size() == 2,
              "单轮即覆盖全体对端（一轮 = 蝶点辐射）");
        check(p && p->rounds[0].edges[0].stage == 0,
              "stage=0：唯一的压缩/辐射轮（不同于蝶式的位翻转阶段号）");
        check(!p->degraded, "c001 无资源需求 → 不发生降级");

        // ② 蝶点物化：源端捕获时显式生成摘要（「压缩成点」的落地）
        rt.layer_enter({"hub"});
        rt.shadow_write("hub", "expect_z", 0.5);
        rt.layer_exit({"hub"});
        check(rt.stats().c001_points == 1, "蝶点摘要已显式物化（c001_points = 1）");
        check(sink.str().find("蝶点 digest") != std::string::npos,
              "蝶点摘要写入诊断（内容可见，不静默）");
        double pay = 0.0;
        check(rt.payload_of("hub", "expect_z", pay) && nearly(pay, 0.5),
              "蝶点持有主要特征摘要 expect_z=0.5");

        // ③ 辐射：两个目标层各在 1 轮内收到（含远端 leaf_b）。
        //    投递值 = 蝶点摘要 × 关联权重 w=exp(−d/λ)（c001 改的是深度与覆盖，
        //    不改加权语义 —— 与其它策略同一套核）。
        const size_t before = rt.stats().deliveries;
        rt.layer_enter({"leaf_a", "leaf_b"});
        const double wa = std::exp(-32.0), wb = std::exp(-64.0);
        double va = 0.0, vb = 0.0;
        check(rt.shadow_first("leaf_a", "expect_z", va) && nearly(va, 0.5 * wa),
              "leaf_a 经蝶点 1 轮即收到（d=32，按 w=exp(−32) 加权）");
        check(rt.shadow_first("leaf_b", "expect_z", vb) && nearly(vb, 0.5 * wb),
              "leaf_b 经蝶点 1 轮即收到（d=64；蝶式此时需 ⌈log₂130⌉=8 轮）");
        check(rt.stats().deliveries == before + 2, "两个对端各投递 1 条边（共 1 轮）");
        rt.layer_exit({"leaf_a", "leaf_b"});
        check(rt.stats().returns == 2, "两个目标各产生 1 次返回腿（双侧 = DNA 双链互补）");
    }

    std::printf("\n=== 结果：%s ===\n", g_fail == 0 ? "全部通过" : "存在失败");
    return g_fail == 0 ? 0 : 1;
}