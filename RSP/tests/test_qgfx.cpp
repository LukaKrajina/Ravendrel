// qgfx / qqui 量子图形与量子 UI 单元测试（不依赖窗口/GPU）
//
// 计算层 qgfx_* 是 headless 可测的；qqui_* 在无画布时安全返回，
// 因此本测试同时覆盖二者的健壮性契约（失效句柄、越界索引、极端参数）。
#include "test_framework.hpp"
#include "qhal/QuantumState.hpp"
#include "qhal/QuantumGfx.hpp"
#include "qhal/QuantumUi.hpp"

#include <cmath>

using namespace qhal;

static const double kSqrtHalf = std::sqrt(0.5);

static void set_bell(uint64_t h)
{
    const double amp[8] = {kSqrtHalf, 0.0, 0.0, 0.0, 0.0, 0.0, kSqrtHalf, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 8, 2);
}

// 直接访问帧（跳过 C ABI），用于断言纯态内部量。
static double collision_of(uint64_t h)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->collision_probability() : -1.0;
}

static double linear_entropy_of(uint64_t h)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->linear_entropy() : -1.0;
}

static bool entangled(uint64_t h, uint32_t qa, uint32_t qb)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->entanglement_exists(qa, qb) : false;
}

static double pair_strength(uint64_t h, uint32_t qa, uint32_t qb)
{
    const QuantumFrame *f = qgfx_resolve_const(h);
    return f ? f->pair_entanglement(qa, qb) : -1.0;
}

// ═══ 帧生命周期与句柄安全 ═══════════════════════════════════════════

QTEST(qgfx_frame_lifecycle) {
    uint64_t h = rvdl_qgfx_frame_new();
    QCHECK(h != 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 1);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 2);
    // 【契约】新帧是零向量（残差 1.0），刻意**不是** |0>。
    // 若这里变成 0.0，说明某处把「无数据」伪装成了真空态。
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 1.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_probability(h, 0), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_stale_handle_is_safe) {
    uint64_t h = rvdl_qgfx_frame_new();
    rvdl_qgfx_frame_release(h);
    // 释放后的句柄必须解析为空，而非野指针解引用
    QCHECK(qgfx_resolve_const(h) == nullptr);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 0);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 0);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 1.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    QCHECK(rvdl_qgfx_quaternionic(h, nullptr) == 0);
    // 重复释放必须幂等
    rvdl_qgfx_frame_release(h);
    // 完全无效的句柄（垃圾值）同样安全
    QCHECK(rvdl_qgfx_frame_num_qubits(0xDEADBEEF) == 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(0) == 0);
}

QTEST(qgfx_slot_reuse_after_release) {
    uint64_t a = rvdl_qgfx_frame_new();
    rvdl_qgfx_frame_release(a);
    uint64_t b = rvdl_qgfx_frame_new();
    // 槽位应被复用（新句柄可解析）
    QCHECK(b != 0);
    QCHECK(qgfx_resolve_const(b) != nullptr);
    rvdl_qgfx_frame_release(b);
}

QTEST(qgfx_capture_without_backend) {
    uint64_t h = rvdl_qgfx_frame_new();
    // 空后端：返回 0，帧保持 |0…0> 且被视为不可信（残差 1.0）
    QCHECK(rvdl_qgfx_frame_capture(h, nullptr, 3) == 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 3);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 8);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 1.0, 1e-12);
    // 非法 qubit 数不得崩溃
    QCHECK(rvdl_qgfx_frame_capture(h, nullptr, 0) == 0);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 1);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_amplitude_clamping) {
    uint64_t h = rvdl_qgfx_frame_new();
    // 未归一化帧必须暴露残差，而非静默归一化
    const double amp[4] = {2.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 2, 2);
    QCHECK(rvdl_qgfx_frame_residual(h) > 0.5);
    // 超量振幅应被截断到 2^num_qubits
    const double many[32] = {1.0};
    rvdl_qgfx_frame_set_amplitudes(h, many, 32, 2);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 4);
    // 越界基态索引安全返回 0
    QCHECK(rvdl_qgfx_probability(h, 999) == 0.0);
    QCHECK(rvdl_qgfx_amplitude(h, 999, 4) == 0.0);
    // 空指针安全
    rvdl_qgfx_frame_set_amplitudes(h, nullptr, 8, 2);
    rvdl_qgfx_bloch(h, 0, nullptr);
    rvdl_qgfx_frame_release(h);
}

// ═══ P1：投影显式化 ═════════════════════════════════════════════════

QTEST(qgfx_projection_introspection) {
    // 状态空间的实维数：dim_R CP^(2^n−1) = 2(2^n−1)
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(1), 2.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(2), 6.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(3), 14.0, 1e-12);

    // 2 比特态 living 在 6 维实空间中。契约：任何投影都不得"增维" ——
    // qgf_projection_target_dims 按**独立参数个数**记账，
    // 冗余坐标（如 entanglement_sphere 画出的 9 个坐标受秩-1 约束）
    // 会被源维数封顶。
    const double src2 = rvdl_qgfx_projection_source_dims(2);
    QCHECK_NEAR(src2, 6.0, 1e-12);
    for (int p = 0; p <= 6; ++p) {
        const double dst = rvdl_qgfx_projection_target_dims(p, 2);
        QCHECK(dst > 0.0);
        QCHECK(dst <= src2);          // 任何投影都不可能增维
        QCHECK(std::string(rvdl_qgfx_projection_name(p)) != "unknown");
    }

    // 无损投影：2 比特的 local_bloch（2×3 = 6）恰好完整
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(0, 2), 6.0, 1e-12);
    // entanglement_sphere 画 9 个坐标但受秩-1 约束 → 记账为 6（封顶）
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(2, 2), 6.0, 1e-12);

    // 严格有损的：Wigner 截面（单个 2D 截面）
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(5, 2), 2.0, 1e-12);
    // 单层嵌套 / 超球只展示当前壳 → 3 维
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(3, 2), 3.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(4, 2), 3.0, 1e-12);

    // 3 比特：源 14 维，local_bloch 9 维 → 明显有损（6 < 14）
    QCHECK_NEAR(rvdl_qgfx_projection_source_dims(3), 14.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(0, 3), 9.0, 1e-12);
    QCHECK(rvdl_qgfx_projection_target_dims(0, 3) < 14.0);
    // 而 2 比特时 local_bloch 无损 —— 证明"有损"是逐 (投影, n) 的结论
    QCHECK_NEAR(rvdl_qgfx_projection_target_dims(0, 2), 6.0, 1e-12);

    // 全部投影号都有名字，非法号报告 unknown
    for (int p = 0; p <= 6; ++p)
        QCHECK(std::string(rvdl_qgfx_projection_name(p)) != "unknown");
    QCHECK(std::string(rvdl_qgfx_projection_name(99)) == "unknown");

    // qubit 数下界保护
    QCHECK(rvdl_qgfx_projection_source_dims(0) > 0.0);
}

// ═══ P2：谓词可读化 ═══════════════════════════════════════════════

QTEST(qgfx_predicates_on_bell) {
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);

    // 碰撞概率 Σ|a_i|⁴：Bell 在 2 比特上均匀分布 → 2×(0.5²) = 0.5。
    // 注意它不是 Tr(ρ²)（对纯态恒为 1）。
    QCHECK_NEAR(collision_of(h), 0.5, 1e-9);
    // 线性熵 S_L = 1 − Σ|a_i|⁴ = 0.5（最大叠加）
    QCHECK_NEAR(linear_entropy_of(h), 0.5, 1e-9);

    // Bell |Φ+> 的 concurrence 精确为 1
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 1.0, 1e-9);
    // 纠缠存在性判据必须为真（这才是可分离性的**充要**判据）
    QCHECK(entangled(h, 0, 1) == 1);

    // 四元数部分 = C/2 ≈ 0 ⟺ 可分离；此处应为 0.5（最大纠缠）
    double q[4] = {0, 0, 0, 0};
    QCHECK(rvdl_qgfx_quaternionic(h, q) == 1);
    QCHECK_NEAR(q[2], 0.5, 1e-9);
    // Schmidt 角 = ½·asin(C) = π/4
    QCHECK_NEAR(q[0], M_PI / 4.0, 1e-9);

    // 边缘态最大混合 → 熵 = 1；整体纯 → 熵 = 0
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 1, 0), 1.0, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 1, 1, 0), 1.0, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 2, 1), 0.0, 1e-9);

    // Bloch：Bell 的每个比特都是最大混合 → θ = π/2
    double bp[2] = {0, 0};
    rvdl_qgfx_bloch(h, 0, bp);
    QCHECK_NEAR(bp[0], M_PI / 2.0, 1e-6);

    // 条码：H₀ 条 {1 → λ_max = 1/√2}
    double bars[8] = {0};
    const int32_t nb = rvdl_qgfx_barcode(h, 0, 1, bars, 4);
    QCHECK(nb >= 1);
    QCHECK_NEAR(bars[0], 1.0, 1e-9);
    QCHECK_NEAR(bars[1], kSqrtHalf, 1e-9);

    // 干项：Re(a00 a11*) = 0.5；与空基态的干涉为 0
    QCHECK_NEAR(rvdl_qgfx_interference(h, 0, 3), 0.5, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_interference(h, 0, 1), 0.0, 1e-12);

    // out_bars == nullptr 时只返回条数
    QCHECK(rvdl_qgfx_barcode(h, 0, 1, nullptr, 0) == nb);

    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_separable_predicate) {
    uint64_t h = rvdl_qgfx_frame_new();
    // 可分离态 |00>：四元数部分必须为 0（P2 的核心可读判据）
    const double amp[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 8, 2);
    double q[4] = {9, 9, 9, 9};
    QCHECK(rvdl_qgfx_quaternionic(h, q) == 1);
    QCHECK_NEAR(q[2], 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 1, 0), 0.0, 1e-12);
    // Bloch：|0> 态 → θ = 0（指向 +Z）
    double bp[2] = {9, 9};
    rvdl_qgfx_bloch(h, 0, bp);
    QCHECK_NEAR(bp[0], 0.0, 1e-9);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_quaternionic_invalid_for_wrong_size) {
    uint64_t h = rvdl_qgfx_frame_new();
    const double amp[4] = {kSqrtHalf, 0.0, kSqrtHalf, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 4, 2);
    // 3 比特上 concurrence 无定义，必须报告无效而非给出误导数字
    const double amp3[16] = {1.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp3, 2, 3);
    double q[4] = {0, 0, 0, 0};
    QCHECK(rvdl_qgfx_quaternionic(h, q) == 0);
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_same_class_equivalence) {
    uint64_t a = rvdl_qgfx_frame_new();
    uint64_t b = rvdl_qgfx_frame_new();
    // 同一 Schmidt 谱（系数对换）→ 同一局域幺正等价类
    const double amp_a[8] = {0.6, 0.0, 0.0, 0.0, 0.0, 0.0, 0.8, 0.0};
    const double amp_b[8] = {0.8, 0.0, 0.0, 0.0, 0.0, 0.0, 0.6, 0.0};
    rvdl_qgfx_frame_set_amplitudes(a, amp_a, 8, 2);
    rvdl_qgfx_frame_set_amplitudes(b, amp_b, 8, 2);
    QCHECK_NEAR(rvdl_qgfx_concurrence(a), rvdl_qgfx_concurrence(b), 1e-9);
    QCHECK(rvdl_qgfx_same_class(a, b, 0, 1, 1e-6) == 1);

    // 可分离 vs 最大纠缠 → 不同等价类
    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(b, prod, 8, 2);
    QCHECK(rvdl_qgfx_same_class(a, b, 0, 1, 1e-6) == 0);

    // 失效句柄参与比较 → false，不得崩溃
    rvdl_qgfx_frame_release(b);
    QCHECK(rvdl_qgfx_same_class(a, b, 0, 1, 1e-6) == 0);
    rvdl_qgfx_frame_release(a);
}

// ═══ P3：复值精确图元 ═════════════════════════════════════════════

QTEST(qgfx_dirichlet_kernel) {
    // l'Hôpital 极限：D_N(0) = 2N+1
    QCHECK_NEAR(QuantumFrame::dirichlet(0.0, 4), 9.0, 1e-6);
    QCHECK_NEAR(QuantumFrame::dirichlet(0.0, 0), 1.0, 1e-12);
    QCHECK_NEAR(QuantumFrame::dirichlet(0.0, 1), 3.0, 1e-6);
    // 偶函数
    QCHECK_NEAR(QuantumFrame::dirichlet(0.7, 4), QuantumFrame::dirichlet(-0.7, 4), 1e-12);
    // 零点：D_3 在 x = 2π/7 处
    QCHECK_NEAR(QuantumFrame::dirichlet(2.0 * M_PI / 7.0, 3), 0.0, 1e-9);
    // 振荡性：主瓣与旁瓣交替（这正是「不能用高斯近似」的原因）
    const double lobe = std::fabs(QuantumFrame::dirichlet(0.0, 8));
    const double side = std::fabs(QuantumFrame::dirichlet(2.0 * M_PI / 17.0, 8));
    QCHECK(lobe > side * 5.0);
}

QTEST(qgfx_phase_is_carried) {
    // 相位是经典直方图永远丢失的信息 —— 本测试确认它确实存在
    // count 是双精度元素个数 = 2 × 2^num_qubits。2 比特 → count 必须为 8。
    // ABI 布局为 (re, im) 交错：基态 k 的实部在 arr[2k]，虚部在 arr[2k+1]。
    //
    // 两个态的**概率分布完全相同**，但相差一个相位：
    //   a = (|00> + |01>)/√2        —— 全实
    //   b = (|00> + i|01>)/√2       —— |01> 分量带 i 相位
    // 经典直方图只看到两个 0.5 的柱，永远无法区分；Bloch 指向却不同。
    const double real_only[8] = {kSqrtHalf, 0.0, kSqrtHalf, 0.0,
                                  0.0, 0.0, 0.0, 0.0};
    const double with_phase[8] = {kSqrtHalf, 0.0, 0.0, kSqrtHalf,
                                  0.0, 0.0, 0.0, 0.0};
    uint64_t a = rvdl_qgfx_frame_new();
    uint64_t b = rvdl_qgfx_frame_new();
    rvdl_qgfx_frame_set_amplitudes(a, real_only, 8, 2);
    rvdl_qgfx_frame_set_amplitudes(b, with_phase, 8, 2);

    // 概率分布完全相同（经典视角无法区分）
    QCHECK_NEAR(rvdl_qgfx_probability(a, 0), rvdl_qgfx_probability(b, 0), 1e-12);
    QCHECK_NEAR(rvdl_qgfx_probability(a, 1), rvdl_qgfx_probability(b, 1), 1e-12);
    // 但相位不同 —— 这正是经典直方图丢失的信息（P3 的立论）
    QCHECK_NEAR(rvdl_qgfx_amplitude_phase(a, 1), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_amplitude_phase(b, 1), M_PI / 2.0, 1e-12);
    // Bloch 指向随之改变。a 的 q0 是 |+⟩（Bloch +x），b 的 q0 是 |+i⟩（Bloch +y）：
    //   a: (x,y,z) = (1, 0, 0) → θ = acos(0) = π/2, φ = atan2(0,1) = 0
    //   b: (x,y,z) = (0, 1, 0) → θ = π/2,            φ = atan2(1,0) = π/2
    // 注意 θ 相同（都在赤道面上），差的是方位角 φ —— 这正是相位的体现。
    double pa[2] = {0, 0}, pb[2] = {0, 0};
    rvdl_qgfx_bloch(a, 0, pa);
    rvdl_qgfx_bloch(b, 0, pb);
    QCHECK_NEAR(pa[0], M_PI / 2.0, 1e-9);      // +x → θ = π/2
    QCHECK_NEAR(pb[0], M_PI / 2.0, 1e-9);      // +y → θ = π/2
    QCHECK_NEAR(pa[1], 0.0, 1e-9);            // φ = 0
    QCHECK_NEAR(pb[1], M_PI / 2.0, 1e-9);      // φ = π/2  ← 相位差在此可见
    QCHECK(std::fabs(pa[1] - pb[1]) > 1e-6);
    rvdl_qgfx_frame_release(a);
    rvdl_qgfx_frame_release(b);
}

QTEST(qgfx_phase_hue_is_perceptual) {
    // 相位 → 色相必须落在 [0,360)（0 有效，360 无效）
    QCHECK(rvdl_qgfx_phase_hue(0.0) >= 0.0 && rvdl_qgfx_phase_hue(0.0) < 360.0);
    QCHECK(rvdl_qgfx_phase_hue(M_PI) >= 0.0 && rvdl_qgfx_phase_hue(M_PI) < 360.0);
    // 相位 ±π 等价（同一复值）
    QCHECK_NEAR(rvdl_qgfx_phase_hue(M_PI), rvdl_qgfx_phase_hue(-M_PI), 1e-9);
    // 0 与 2π 等价 —— 浮点上 atan2(sin,cos) 可能给出极小的负值，
    // 归一化后恰好落到 360.0，故用环绕距离而非绝对差比较。
    {
        const double a = rvdl_qgfx_phase_hue(0.0);
        const double b = rvdl_qgfx_phase_hue(2.0 * M_PI);
        const double circ = std::fabs(a - b);
        QCHECK_NEAR(std::min(circ, 360.0 - circ), 0.0, 1e-6);
    }
    // 环绕一致性：色相 0 与 360 必须是同一个视觉端点
    QCHECK(rvdl_qgfx_phase_hue(1e-15) < 360.0);
    QCHECK(rvdl_qgfx_phase_hue(-1e-15) < 360.0);
    // 等角步进给出等距色相（感知均匀的基本性质）。在**圆周**上度量：
    // 三等分点 0 / 2π/3 / 4π/3 之间的弧长必须都相同。
    {
        const double h0 = rvdl_qgfx_phase_hue(0.0);
        const double h1 = rvdl_qgfx_phase_hue(2.0 * M_PI / 3.0);
        const double h2 = rvdl_qgfx_phase_hue(4.0 * M_PI / 3.0);
        auto arc = [](double from, double to) {
            const double d = std::fabs(to - from);
            return std::min(d, 360.0 - d);
        };
        QCHECK_NEAR(arc(h0, h1), 120.0, 1e-6);
        QCHECK_NEAR(arc(h1, h2), 120.0, 1e-6);
        QCHECK_NEAR(arc(h2, h0), 120.0, 1e-6);
    }
}

// ═══ P4：嵌套层级 ═════════════════════════════════════════════════

QTEST(qgfx_nested_hierarchy) {
    QCHECK(rvdl_qgfx_nested_shells(1) == 1);
    QCHECK(rvdl_qgfx_nested_shells(2) == 2);
    QCHECK(rvdl_qgfx_nested_shells(3) == 4);
    QCHECK(rvdl_qgfx_nested_shells(4) == 8);
    // 层级是数据结构的内蕴属性：20 比特仅 2^19 壳，可按需裁剪
    QCHECK(rvdl_qgfx_nested_shells(20) == 524288);

    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);
    // 壳质量守恒：全部壳之和为 1
    double total = 0.0;
    for (int32_t s = 0; s < 2; ++s)
        total += rvdl_qgfx_nested_mass(h, s, 2);
    QCHECK_NEAR(total, 1.0, 1e-9);
    // 越界壳安全返回 0
    QCHECK(rvdl_qgfx_nested_mass(h, 5, 2) == 0.0);
    QCHECK(rvdl_qgfx_nested_mass(h, -1, 2) == 0.0);
    rvdl_qgfx_frame_release(h);
}

// ═══ qqui_*：无画布时的健壮性契约 ═══════════════════════════════════

QTEST(qqui_headless_no_crash) {
    // 这些调用在无 RenderContext 时必须安全返回/返回哨兵值。
    // 若某处解引用了空画布，测试会崩而不是给出断言失败 —— 这正是契约。
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);

    rvdl_qqui_qubit_glyph(0, 0, 24, h, 0, 0xFFFFFF, 255);
    rvdl_qqui_qubit_row(0, 0, 24, h, 0, 4, 0xFFFFFF, 255);
    const double link = rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255);
    // 谓词在无画布时仍必须返回真实数值（供调用方做断言）
    QCHECK_NEAR(link, 1.0, 1e-9);

    rvdl_qqui_uncertainty_bin(0, 100, 20, 0.5, 0.1, 0x00FF00, 255);
    rvdl_qqui_wigner_slice(0, 0, 64, 64, h, 0, 16);
    rvdl_qqui_nested_bar(0, 0, 8, h, 0x0000FF, 255);
    QCHECK(rvdl_qqui_amplitude_strip(0, 0, 100, 50, h, 0xFFFFFF, 255) == -1);
    rvdl_qqui_state_card(0, 0, 200, h, 1, 0xFFFFFF, 255);

    // 退化输入不得崩溃
    rvdl_qqui_qubit_glyph(0, 0, 0, h, 0, 0xFFFFFF, 255);
    rvdl_qqui_qubit_glyph(0, 0, -5, h, -1, 0xFFFFFF, 255);
    rvdl_qqui_uncertainty_bin(0, 0, 0, 0.0, 0.0, 0, 0);
    rvdl_qqui_wigner_slice(0, 0, 0, 0, h, 0, 16);
    rvdl_qqui_wigner_slice(0, 0, 64, 64, h, 0, 0);   // res 非法 → 应被钳制
    rvdl_qqui_wigner_slice(0, 0, 64, 64, h, 0, 9999);
    rvdl_qqui_nested_bar(0, 0, 0, h, 0, 0);
    rvdl_qqui_state_card(0, 0, 0, h, 0, 0, 0);

    // 失效句柄：所有控件都必须静默跳过
    rvdl_qgfx_frame_release(h);
    rvdl_qqui_qubit_glyph(0, 0, 24, h, 0, 0xFFFFFF, 255);
    QCHECK(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255) == 0.0);
    QCHECK(rvdl_qqui_amplitude_strip(0, 0, 100, 50, h, 0, 0) == -1);
    rvdl_qqui_nested_bar(0, 0, 8, h, 0, 0);
    rvdl_qqui_state_card(0, 0, 200, h, 1, 0, 0);
}

QTEST(qqui_link_edge_cases) {
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);
    // 同一比特的连线无意义，必须返回 0
    QCHECK(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 1, 1, 0xFF0000, 255) == 0.0);
    // 可分离态：纠缠读数为 0（由颜色/宽度编码为低注意力，而非隐藏）
    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, prod, 8, 2);
    QCHECK(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255) == 0.0);
    rvdl_qgfx_frame_release(h);
}

// ═══ 深层帧：3 比特以上的谓词正确性 ═══════════════════════════════

QTEST(qgfx_three_qubit_ghz) {
    uint64_t h = rvdl_qgfx_frame_new();
    // GHZ3 = (|000> + |111>)/√2
    const double amp[16] = {kSqrtHalf, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                             0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, kSqrtHalf};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 16, 3);
    QCHECK(rvdl_qgfx_frame_basis_size(h) == 8);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 0.0, 1e-12);
    // 纯态总熵为 0
    QCHECK_NEAR(rvdl_qgfx_entropy(h, 0, 3, 1), 0.0, 1e-9);
    // 每个比特的边缘都是最大混合 → 熵 1
    for (int32_t q = 0; q < 3; ++q)
        QCHECK_NEAR(rvdl_qgfx_entropy(h, q, 1, 0), 1.0, 1e-9);
    // 条码：q0|q1 二分给出秩 2（GHZ 与全体二分）
    double bars[8] = {0};
    const int32_t nb = rvdl_qgfx_barcode(h, 0, 1, bars, 4);
    QCHECK(nb >= 1);
    QCHECK_NEAR(bars[1], kSqrtHalf, 1e-9);
    // 3 比特上 concurrence 无定义
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

    QTEST(qgfx_n_qubit_pair_conventions) {
    // n>2 时 concurrence 无定义。本 Frame 采取的**明确契约**：
    //   - entanglement_exists(qa,qb) → 二值判据（Schmidt 秩，纯态下充要）
    //   - pair_entanglement(qa,qb)   → **恒返回 0**，标记为「不可用」
    // 刻意不外推出一个似是而非的连续强度：任何只依赖 Schmidt 谱单参数差
    // 的度量都会把 Bell（谱 {1/√2,1/√2}，差为 0）误报为可分离。
    // 宁可不提供，也不提供错的 —— 否则 UI 会悄悄画出错误纠缠强度。
    uint64_t h = rvdl_qgfx_frame_new();
    // Bell 态位于 q0、q1，q2 固定为 0 → (|000> + |110>)/√2。
    // ABI 为 (re, im) 交错：基态索引 6（二进制 110）的实部在下标 12。
    const double amp[16] = {kSqrtHalf, 0.0,   // |000>
                             0.0, 0.0,
                             0.0, 0.0,
                             0.0, 0.0,
                             0.0, 0.0,
                             kSqrtHalf, 0.0,   // |110>
                             0.0, 0.0,
                             0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, amp, 16, 3);
    QCHECK(rvdl_qgfx_frame_num_qubits(h) == 3);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(h), 0.0, 1e-12);

    // 3 比特上 concurrence 无定义 → 报告 0 而非编造数字
    QCHECK_NEAR(rvdl_qgfx_concurrence(h), 0.0, 1e-12);
    // 纠缠强度在 n>2 明确不可用
    QCHECK_NEAR(pair_strength(h, 0, 1), 0.0, 1e-12);
    // 但二值判据正常工作：q0|q1 纠缠，q0|q2 可分离
    QCHECK(entangled(h, 0, 1) == true);
    QCHECK(entangled(h, 0, 2) == false);

    // 完全可分离的 3 比特态：所有判据都应为「无纠缠」
    const double prod[16] = {1.0}; // |000>
    rvdl_qgfx_frame_set_amplitudes(h, prod, 16, 3);
    QCHECK(entangled(h, 0, 1) == false);
    QCHECK(entangled(h, 0, 2) == false);
    QCHECK_NEAR(pair_strength(h, 0, 1), 0.0, 1e-12);

    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_two_qubit_pair_strength) {
    // 2 比特时 pair_entanglement 精确等于 concurrence（唯一有效情形）
    uint64_t h = rvdl_qgfx_frame_new();
    set_bell(h);
    QCHECK_NEAR(pair_strength(h, 0, 1), 1.0, 1e-9);
    QCHECK(entangled(h, 0, 1) == true);

    // 可分离态：强度必须为 0（语义正确，UI 才不会误画纠缠连线）
    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(h, prod, 8, 2);
    QCHECK_NEAR(pair_strength(h, 0, 1), 0.0, 1e-12);
    QCHECK(entangled(h, 0, 1) == false);
    // 连线控件返回 0（可分离 → 灰色低注意力，而非画成纠缠）
    QCHECK_NEAR(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 0, 1, 0xFF0000, 255), 0.0, 1e-12);
    // 同一比特的连线无意义
    QCHECK_NEAR(rvdl_qqui_entangle_link(0, 0, 100, 0, h, 1, 1, 0xFF0000, 255), 0.0, 1e-12);
    rvdl_qgfx_frame_release(h);
}

QTEST(qgfx_ghz_vs_w_spectrum) {
    // GHZ3 与 W 态：边缘熵不同，但 Schmidt 秩相同（二者皆为 2）。
    // 这说明单一边缘量与单一秩都不足以刻画纠缠，必须保留完整谱。
    //
    // 对照组：一个完全可分离的纯乘积态 |0>⊗|+>⊗|0>，用于验证
    // 「纯态下全部单比特边缘最大混合 ⟺ 多体纠缠」（Koenig–Winter /
    // Gühne–Vidal 判据），以及 Schmidt 秩判据在二分上的一致性。
    uint64_t ghz = rvdl_qgfx_frame_new();
    uint64_t w = rvdl_qgfx_frame_new();
    // GHZ3 = (|000> + |111>)/√2，索引 0 与 7
    // 索引 7 的实部在下标 14，务必写成 (S, 0) 而非 (0, S) ——
    // 后者会引入一个 i 相位（仍是合法态，但与预期态不同）。
const double g_amp[16] = {kSqrtHalf, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0, 0.0, 0.0, kSqrtHalf, 0.0};
    // W 态 = (|001> + |010> + |100>)/√3 → 基态索引 1、2、4 有非零实部。
    // 注意 ABI 布局是 (re, im) 交错，故基态 k 的实部位于下标 2k。
    const double wt = 1.0 / std::sqrt(3.0);
    const double w_amp[16] = {0.0, 0.0,   // |000> = 0
                               wt, 0.0,   // |001> = wt
                               wt, 0.0,   // |010> = wt
                               0.0, 0.0,   // |011> = 0
                               wt, 0.0,   // |100> = wt
                               0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(ghz, g_amp, 16, 3);
    rvdl_qgfx_frame_set_amplitudes(w, w_amp, 16, 3);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(ghz), 0.0, 1e-12);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(w), 0.0, 1e-12);

    // 边缘熵：GHZ 的每个比特都是最大混合（S = 1）；W 的每个比特给出
    // H(2/3, 1/3) ≈ 0.918 —— 比 1 小，说明边缘**不是**最大混合。
    // 这条差异本身也是可读的物理信息，故一并断言。
    for (int32_t q = 0; q < 3; ++q) {
        QCHECK_NEAR(rvdl_qgfx_entropy(ghz, q, 1, 0), 1.0, 1e-9);
        QCHECK_NEAR(rvdl_qgfx_entropy(w, q, 1, 0), 0.9182958342, 1e-8);
    }

    // Schmidt 谱的**形状**不同，故条码判据可区分二者。
    //
    // 【秩的事实】1|2 二分的 Schmidt 秩上限为 min(2¹, 2²) = 2，因此
    // GHZ 与 W 在这个二分上**秩相同**（均为 2）—— "GHZ 秩 2 / W 秩 3"
    // 的说法是错的：3 比特态在单比特切分下秩不可能超过 2。
    // 区分二者的是谱的数值形状：
    //   GHZ: {1/√2, 1/√2}     （最大纠缠）
    //   W:   {√(2/3), √(1/3)} （非最大纠缠）
    double gb[16], wb[16];
    const int32_t ng = rvdl_qgfx_barcode(ghz, 0, 1, gb, 8);
    const int32_t nw = rvdl_qgfx_barcode(w, 0, 1, wb, 8);
    QCHECK(ng == 2);
    QCHECK(nw == 2);                                // 秩相同 —— 不是区分依据
    QCHECK_NEAR(gb[1], kSqrtHalf, 1e-9);
    QCHECK_NEAR(wb[1], std::sqrt(2.0 / 3.0), 1e-9); // 谱形状才是区分依据
    QCHECK(rvdl_qgfx_same_class(ghz, w, 0, 1, 1e-6) == 0);

    // 两者都被判定为「存在纠缠」，但边缘熵已能区分 → 条码提供了
    // 一个不依赖熵的独立判据（可分离性只看秩，见下方 sep 断言）。

    // ── 固定已知局限：边缘熵不足以判定可分离性 ────────────────────
    // |0+00> + |1+00>)/√2 完全可分离（= |0> ⊗ |+> ⊗ |0>），
    // 但它的三比特边缘熵与 GHZ3 完全相同（皆为 1）。
    // 因此判据**必须**走 Schmidt 秩，不能走边缘熵。
    //
    // 态构造：(|0> ⊗ |+> ⊗ |0>)/√2，基态索引 0、1、4。
    // ABI 为 (re, im) 交错，故基态 k 的实部位于下标 2k。
    uint64_t sep = rvdl_qgfx_frame_new();
    // 态：|0> ⊗ |+> ⊗ |0> = (|000> + |010>)/√2 —— 完全可分离（纯乘积态）。
    // 只有 q1 的边缘是混合的；q0、q2 的边缘都是纯态。
    const double s_amp[16] = {kSqrtHalf, 0.0,   // |000>
                               0.0, 0.0,
                               kSqrtHalf, 0.0,   // |010>
                               0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(sep, s_amp, 16, 3);
    QCHECK_NEAR(rvdl_qgfx_frame_residual(sep), 0.0, 1e-12);

    // 边缘熵：纯乘积态 |0>⊗|+>⊗|0> 的**所有**边缘都是纯态（S = 0）。
    // 这是必然的：|a>⊗|b>⊗|c> 的任一边缘约化都是某个 |x>⟨x|。
    for (int32_t q = 0; q < 3; ++q)
        QCHECK_NEAR(rvdl_qgfx_entropy(sep, q, 1, 0), 0.0, 1e-9);
    // GHZ3 的三个边缘**全**是最大混合（S = 1）—— 这正是多体纠缠的标志。
    // 对纯态而言这是充要条件（Koenig–Winter / Gühne–Vidal 判据）：
    // 全部单比特边缘最大混合 ⟺ 不存在任何可分离的二分。
    for (int32_t q = 0; q < 3; ++q)
        QCHECK_NEAR(rvdl_qgfx_entropy(ghz, q, 1, 0), 1.0, 1e-9);
    // 二者的 q0|q1 二分：可分离态秩 1（谱 {1}），GHZ 秩 2
    double sb[16];
    const int32_t ns = rvdl_qgfx_barcode(sep, 0, 1, sb, 8);
    QCHECK(ns == 1);
    QCHECK_NEAR(sb[1], 1.0, 1e-9);
    QCHECK(entangled(sep, 0, 1) == false);
    QCHECK(entangled(ghz, 0, 1) == true);
    QCHECK(rvdl_qgfx_same_class(sep, ghz, 0, 1, 1e-6) == 0);
    rvdl_qgfx_frame_release(sep);

    rvdl_qgfx_frame_release(ghz);
    rvdl_qgfx_frame_release(w);
}

// ═══ 帧隔离：多帧互不干扰 ═════════════════════════════════════════

QTEST(qgfx_frame_isolation) {
    uint64_t a = rvdl_qgfx_frame_new();
    uint64_t b = rvdl_qgfx_frame_new();
    set_bell(a);
    const double prod[8] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    rvdl_qgfx_frame_set_amplitudes(b, prod, 8, 2);
    // 两帧应保持各自的态（P5：一次模拟多视图，但视图彼此独立）
    QCHECK_NEAR(rvdl_qgfx_concurrence(a), 1.0, 1e-9);
    QCHECK_NEAR(rvdl_qgfx_concurrence(b), 0.0, 1e-12);
    // 视图切换零重算：重复读取同一帧结果稳定
    const double r1 = rvdl_qgfx_entropy(a, 0, 1, 0);
    const double r2 = rvdl_qgfx_entropy(a, 0, 1, 0);
    QCHECK_NEAR(r1, r2, 1e-15);
    // 释放一帧不影响另一帧
    rvdl_qgfx_frame_release(a);
    QCHECK_NEAR(rvdl_qgfx_concurrence(b), 0.0, 1e-12);
    rvdl_qgfx_frame_release(b);
}
