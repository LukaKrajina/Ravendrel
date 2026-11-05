#pragma once
// QVM（量子虚拟机）温控模拟子系统。
//
// 对读取经典计算机的 CPU / GPU 温度，通过热模型算法推演出「量子虚拟
// 机芯片」的温度：
//   1) 热负载  L
//   2) 稳态温度 T_ss
//   3) 一阶热系统 T_qvm 以时间常数 τ 指数逼近 T_ss
//   4) 叠加微小量子涨落噪声（模拟真实量子芯片的温度抖动）
//
// CPU/GPU 温度优先真实读取（Linux: /sys/class/thermal + /sys/class/hwmon；
// Windows: WMI 读 CPU + NVML 动态加载读 GPU），读取失败时回退到合成读数。
// 平台相关的真实读取实现在 src/ThermalSimulation.cpp。

#include <algorithm>
#include <cmath>
#include <random>


namespace qhal {

// ─── 宿主机热探针：读取经典 CPU / GPU 温度（°C） ─────────────────
class HostThermalProbe {
public:
  struct Reading {
    double cpu_c = 0.0;
    double gpu_c = 0.0;
    bool synthetic = true; // 是否为合成读数（读取失败时true）
  };

  static Reading sample();

private:
  static Reading sample_linux();
  static Reading sample_windows();
  static Reading synthetic();
};

// ─── QVM 热模型 ─────────────────────────────────────────────
class QVMThermalModel {
private:
  double ambient_c = 25.0; // 环境温度
  double tau_s = 12.0;     // 热时间常数
  double k_load = 0.9;     // 负载耦合系数
  double w_cpu = 0.6, w_gpu = 0.4;
  double t_c = ambient_c;
  std::mt19937 rng{42u};

  double steady_state(double load_c) const {
    return ambient_c + k_load * std::max(0.0, load_c - ambient_c);
  }

public:
  double step(double dt_seconds) {
    HostThermalProbe::Reading r = HostThermalProbe::sample();
    double load = w_cpu * r.cpu_c + w_gpu * r.gpu_c;
    double alpha = 1.0 - std::exp(-dt_seconds / tau_s);
    t_c += alpha * (steady_state(load) - t_c);
    std::normal_distribution<double> jitter(0.0, 0.05);
    t_c += jitter(rng);
    return t_c;
  }

  double celsius() const { return t_c; }
  double kelvin() const { return t_c + 273.15; }
};

class VirtualThermalController {
private:
  QVMThermalModel model;
  double target_c = 25.0;

public:
  // 采样宿主温度并更新 QVM 温度
  void update(double dt_seconds = 1.0) { model.step(dt_seconds); }

  double read_celsius() const { return model.celsius(); }
  double read_kelvin() const { return model.kelvin(); }

  void set_target_c(double t) { target_c = t; }
  double target_celsius() const { return target_c; }
};
}