#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace qhal {
  namespace oram_params {
  constexpr double TPUMP_US = 10.0;     // 泵浦脉冲宽度
  constexpr double TPROBE_US = 10.0;    // 探测脉冲宽度
  constexpr double TSWITCH_US = 2.0;    // 切换时间
  constexpr double TWAIT_US = 3.0;      // 等待时间
  constexpr double TPP_US = 30.0;       // 单次读写（泵浦-探测）周期
  constexpr double T1_US = 230.0;       // 1/e 记忆寿命（扩散限制）
  constexpr double PSAT_MW = 1.5;       // 饱和泵浦功率
  constexpr double PMAX_MW = 1.5;       // 最大泵浦功率
  constexpr size_t NUM_RAILS = 8;       // 可寻址记忆轨道数
  constexpr double BITS_PER_RAIL = 3.8; // 每轨道有效位深（14.5 个可分辨能级）
  constexpr double CELL_TEMP_C = 30.0;  // Cs 气室工作温度（°C）
  constexpr double F0_MHZ = 75.0;       // AOM 起始载频
  constexpr double FSTEP_MHZ = 3.0;     // 载频间隔（避免串扰）
  }

  struct SaturationTransferFunction {
    double a = 1.0;
    double b = 0.0;
    double psat_mw = oram_params::PSAT_MW;

    double operator()(double pump_mw) const {
      return a * pump_mw / (1.0 + pump_mw / psat_mw) + b;
    }
  };

  class AOMDeflector {
    private:
      std::vector<double> freqs_mhz;

    public:
      AOMDeflector() {
        freqs_mhz.reserve(oram_params::NUM_RAILS);
        for (size_t i = 0; i < oram_params::NUM_RAILS; ++i)
          freqs_mhz.push_back(oram_params::F0_MHZ + i * oram_params::FSTEP_MHZ);
      }

      void address(size_t rail) const {
        if (rail < freqs_mhz.size())
          std::cout << "[ORAM.AOM] Addressing rail " << rail << " @ "
                    << freqs_mhz[rail] << " MHz\n";
      }

      double rail_frequency(size_t rail) const {
        return rail < freqs_mhz.size() ? freqs_mhz[rail] : 0.0;
      }
  };

  struct MemoryRailState {
    double effective_pump_mw = 0.0;
    size_t last_write_step = 0;

    double decay(size_t current_step) const {
      double dk = static_cast<double>(current_step - last_write_step);
      return std::exp(-dk * oram_params::TPP_US / oram_params::T1_US);
    }
  };

  class IORAM {
    public:
      virtual ~IORAM() = default;

      virtual void write_rail(size_t rail, double value) = 0; // 泵浦写入
      virtual double read_rail(size_t rail) const = 0;        // 探测读出
      virtual void reset_rail(size_t rail) = 0;          // 主动复位（repumper）
      virtual void passive_reset(double elapsed_us) = 0; // 被动复位（扩散）
      virtual void step() = 0;                           // 推进一个读写周期
      virtual size_t num_rails() const = 0;
      virtual void set_input_scaling(double g1) = 0;
      virtual void inject_reservoir_input(double masked_value, size_t m) = 0;
      virtual double collect_reservoir_output(size_t m) const = 0;
  };

  namespace oram_detail {
    bool invert_matrix(std::vector<std::vector<double>> a,
                      std::vector<std::vector<double>> &inv);
    std::vector<double> jacobi_eigenvalues(std::vector<std::vector<double>> A,
                                          size_t max_iter = 200,
                                          double tol = 1e-10);
  }

  class AtomicVaporORAM : public IORAM {
  private:
    AOMDeflector aom;
    SaturationTransferFunction theta;
    std::vector<MemoryRailState> rails;
    std::vector<std::vector<double>> kin;  // 输入耦合矩阵 K_in [rail][m]
    std::vector<std::vector<double>> kout; // 输出耦合矩阵 K_out [rail][m]
    size_t step_ = 0;
    double g1 = 1.0; // 输入非线性缩放因子

  public:
    explicit AtomicVaporORAM(size_t num_rails = oram_params::NUM_RAILS)
        : rails(num_rails) {
      kin.assign(num_rails, std::vector<double>(num_rails, 0.0));
      kout.assign(num_rails, std::vector<double>(num_rails, 0.0));
      for (size_t i = 0; i < num_rails; ++i) {
        kin[i][i] = 1.0;
        kout[i][(i + 1) % num_rails] = 1.0;
      }
      std::cout << "[ORAM] Atomic vapor ORAM online: " << num_rails << " rails, "
                << oram_params::BITS_PER_RAIL << " bit/rail.\n";
    }

    void write_rail(size_t rail, double value) override {
      if (rail >= rails.size())
        return;
      aom.address(rail);
      double pump = std::clamp(g1 * value * oram_params::PMAX_MW, 0.0,
                              oram_params::PMAX_MW);
      auto &r = rails[rail];
      r.effective_pump_mw = r.effective_pump_mw * r.decay(step_) + pump;
      r.last_write_step = step_;
    }

    double read_rail(size_t rail) const override {
      if (rail >= rails.size())
        return 0.0;
      double attenuated =
          rails[rail].effective_pump_mw * rails[rail].decay(step_);
      return theta(attenuated);
    }

    void reset_rail(size_t rail) override {
      if (rail >= rails.size())
        return;
      rails[rail].effective_pump_mw = 0.0;
      rails[rail].last_write_step = step_;
    }

    void passive_reset(double elapsed_us) override {
      for (auto &r : rails)
        r.effective_pump_mw *= std::exp(-elapsed_us / oram_params::T1_US);
    }

    void step() override { ++step_; }

    size_t num_rails() const override { return rails.size(); }

    void set_input_scaling(double g) override { g1 = g; }

    void inject_reservoir_input(double masked_value, size_t m) override {
      size_t n_rails = rails.size();
      for (size_t n = 0; n < n_rails; ++n) {
        double coupling = kin[n][m % n_rails];
        auto &r = rails[n];
        r.effective_pump_mw =
            coupling * g1 * masked_value * oram_params::PMAX_MW +
            r.effective_pump_mw * r.decay(step_);
        r.last_write_step = step_;
      }
    }

    double collect_reservoir_output(size_t m) const override {
      size_t n_rails = rails.size();
      double out = 0.0;
      for (size_t n = 0; n < n_rails; ++n) {
        double coupling = kout[n][m % n_rails];
        double attenuated =
            coupling * rails[n].effective_pump_mw * rails[n].decay(step_);
        out += theta(attenuated);
      }
      return out;
    }

    static std::vector<double>
    ridge_regression(const std::vector<std::vector<double>> &S,
                    const std::vector<double> &y, double lambda_reg = 5e-6);

    static double
    linear_memory_capacity(const std::vector<std::vector<double>> &S,
                          const std::vector<double> &u, size_t n_lim,
                          double lambda_reg = 5e-6);

    static double kernel_rank(const std::vector<std::vector<double>> &S,
                              double threshold_frac = 0.02);

    static double xor_bit_error_rate(const std::vector<double> &predictions,
                                    const std::vector<int> &truth);
  };
}