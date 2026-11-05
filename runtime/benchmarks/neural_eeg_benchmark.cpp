// ============================================================================
// qbNs/rvdlRSP ：皮层神经场 + EEG 谱分析
//
//   [1] 神经场对称性破缺：初始 φ≈0（不稳定）→ 双稳态破缺，|m| 增长
//   [2] EEG 谱分析：合成 10Hz 正弦，alpha 频段(8-13Hz)应占优
// ============================================================================

#include <cstdio>
#include <cmath>
#include <vector>
#include "spacetime/NeuralField.hpp"
#include "spacetime/SpectralEEG.hpp"

using namespace rvdl::spacetime;

namespace
{
    constexpr double PI = 3.14159265358979323846;

    // ─── 神经场对称性自发破缺 ──────────────────────────────
    bool test_neural_field()
    {
        NeuralFieldParams p;
        NeuralField nf(p);

        double m0 = nf.order_parameter().mean_field;
        for (int s = 0; s < 2000; ++s)
            nf.step();
        auto op = nf.order_parameter();

        printf("  mean_field: %.4f -> %.4f,  破缺|m|=%.4f,  畴壁密度=%.4f\n",
               m0, op.mean_field, op.symmetry_breaking, op.domain_wall_density);
        return op.symmetry_breaking > 0.3;
    }

    // ─── EEG 谱分析：alpha 频段占优 ────────────────────────
    bool test_spectral_eeg()
    {
        // 合成 10Hz 正弦（落在 alpha 8-13Hz）
        std::vector<double> sig(1024);
        for (size_t i = 0; i < sig.size(); ++i)
            sig[i] = std::sin(2.0 * PI * 10.0 * static_cast<double>(i) / 1000.0);

        auto ps = spectral_analysis(sig, 1000.0);
        double alpha_ratio = ps.alpha / (ps.total_power + 1e-12);
        printf("  alpha=%.4f, total=%.4f, alpha 占比=%.3f (期望≈1)\n",
               ps.alpha, ps.total_power, alpha_ratio);
        return alpha_ratio > 0.9;
    }

}

int main()
{
    printf("=== qbNs/rvdlRSP 升级基准 ===\n\n");

    int pass = 0, total = 0;
    bool r;

    printf("[1] 皮层神经场对称性破缺（S² 谱方法）\n");
    r = test_neural_field();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("[2] EEG 精细谱分析（FFT）\n");
    r = test_spectral_eeg();
    printf("    => %s\n\n", r ? "PASS" : "FAIL");
    total++;
    pass += r;

    printf("=== 结果：%d/%d 通过 ===\n", pass, total);
    return pass == total ? 0 : 1;
}