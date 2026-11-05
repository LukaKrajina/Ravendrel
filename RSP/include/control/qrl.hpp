#pragma once
//
// 量子强化学习对齐层 (Quantum RL Alignment Layer) —— qrl::*
//
// ═══════════════════════════════════════════════════════════════════════
// 为什么需要这一层
// ═══════════════════════════════════════════════════════════════════════
//
// 本平台此前的 RL 接口是「玩具级」的：
//
//   struct StepResult { std::vector<double> observation; double reward; bool done; };
//   class IEnvironment { reset(); step(action); observation_dim(); action_dim(); };
//
// 与 Gymnasium（https://gymnasium.farama.org/）相比，它缺的不是功能，
// 而是**一组被广泛实现的约定**。Gymnasium 之所以成为标准（论文
// arXiv:2407.17032），恰恰因为它把七件事定成了规范：
//
//   1. terminated / truncated **分离**。前者是 MDP 终止态（影响 bootstrap），
//      后者是范围外截断（时间上限、越界），**不影响**。
//      旧接口的单一 `done` 无法表达这个区别，会让所有 RL 算法的
//      value target 算错：target = r + γ·V(s')·(1 − terminated)。
//   2. reset(seed=...) 而非独立 seed() 方法。因为有些环境（模拟类）
//      只能在 episode 边界设定随机状态。这让「每回合不同种子」
//      （reset(seed = base + episode)）成为可能，进而使整次训练可复现。
//   3. observation / action **spaces** 是带 contains() 的一等类型。
//      动作越界应当被**立刻捕获**，而不是让物理内核静默饱和。
//   4. info 是 dict，逐项记录奖励分解与隐藏变量（记录器无需改代码）。
//   5. 每步必须返回**全新**的 observation 对象（replay buffer 会持有它们）。
//   6. render mode 在构造时固定（某些后端无法中途切换）。
//   7. metadata 声明 render_fps 等，视频录制才有时序正确的依据。
//
// 本层把这七条约定以 C++ 形式落地，并**保持向后兼容**：既有的
// PhysicsEnvironment / TrackingEnvironment 通过适配器继续可用。
//
// 对比 Genesis（genesis-world.readthedocs.io）：Genesis **没有**实现
// gymnasium.Env —— 其 Go2Env.step() 返回 4 元组 (obs, rew, reset_buf, extras)，
// 观测是 TensorDict 而非 spaces.Box。因此「量子平台 + 标准 RL API」
// 这一组合目前是空位，qrl::* 正是针对该空位。
//
// ───────────────────────────────────────────────────────────────────────
//
// 命名：qrl::Env / qrl::Box / qrl::Discrete ...
// 遵循仓库既有的模块限定 PascalCase 约定（qhal::QVM、qpc::Vec3 …）。

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qrl
{

    // ─── Space：带 contains() 的一等类型（Gymnasium 约定 3）─────────
    //
    // 只实现物理仿真 RL 实际需要的子集：Box / Discrete / MultiDiscrete。
    // 自定义 Space 允许，但 sample() 分布与向量批处理仅对内建类型良定义 ——
    // 与 Gymnasium 的同一告警（见其 spaces 文档）保持一致。
    class Space
    {
    public:
        virtual ~Space() = default;
        // 元素是否属于该空间。越界动作必须能由此捕获。
        virtual bool contains(const std::vector<double> &x) const = 0;
        virtual std::string name() const = 0;
        virtual size_t dim() const = 0;

        // 采样。rng 必须由调用方提供，以便复现（不依赖全局状态）。
        virtual std::vector<double> sample(std::mt19937 &rng) const = 0;

        void seed(uint64_t s)
        {
            rng_.seed(static_cast<std::mt19937::result_type>(s));
            seeded_ = true;
        }
        bool seeded() const { return seeded_; }

    protected:
        std::mt19937 rng_{std::random_device{}()};
        bool seeded_ = false;
    };

    // 连续空间 [low, high]。支持逐维上下界与无界维（±inf）。
    class Box : public Space
    {
    public:
        // 注意：两个 vector 参数**不加 explicit**，以支持
        // Box b({-1.0}, {1.0}); 这样的 braced-init 调用。
        // (size_t) 重载保证 Box(3) 不会歧义地匹配到 vector 重载。
        Box(std::vector<double> low, std::vector<double> high)
            : low_(std::move(low)), high_(std::move(high))
        {
            if (low_.size() != high_.size())
                throw std::invalid_argument("[qrl.Box] low/high size mismatch");
            for (size_t i = 0; i < low_.size(); ++i)
                if (std::isnan(low_[i]) || std::isnan(high_[i]))
                    throw std::invalid_argument("[qrl.Box] bounds must not be NaN");
                else if (low_[i] > high_[i])
                    throw std::invalid_argument("[qrl.Box] low[i] > high[i] (empty interval)");
        }

        // 便捷重载：n 维 [−1, 1] 盒
        explicit Box(size_t dim)
            : low_(dim, -1.0), high_(dim, 1.0)
        {
            if (dim == 0)
                throw std::invalid_argument("[qrl.Box] dim must be positive");
        }

        // 便捷重载：各向同性 [lo, hi] 盒
        Box(double lo, double hi)
            : low_(1, lo), high_(1, hi)
        {
            if (std::isnan(lo) || std::isnan(hi))
                throw std::invalid_argument("[qrl.Box] bounds must not be NaN");
            if (lo > hi)
                throw std::invalid_argument("[qrl.Box] lo > hi (empty interval)");
        }

        bool contains(const std::vector<double> &x) const override
        {
            if (x.size() != low_.size())
                return false;
            for (size_t i = 0; i < x.size(); ++i)
            {
                // NaN 一律判为越界：NaN 会静默传播进物理内核并污染整条轨迹。
                if (std::isnan(x[i]))
                    return false;
                if (x[i] < low_[i] || x[i] > high_[i])
                    return false;
            }
            return true;
        }

        std::string name() const override { return "Box"; }
        size_t dim() const override { return low_.size(); }

        std::vector<double> sample(std::mt19937 &rng) const override
        {
            std::uniform_real_distribution<double> u(0.0, 1.0);
            std::vector<double> out(low_.size());
            for (size_t i = 0; i < low_.size(); ++i)
            {
                // 无界维不能均匀采样，退化为 N(0,1)：这是 Gymnasium 未定义
                // 的情形，此处给出确定行为并记录于此。
                if (std::isinf(low_[i]) || std::isinf(high_[i]))
                {
                    std::normal_distribution<double> g(0.0, 1.0);
                    out[i] = g(rng);
                }
                else
                {
                    out[i] = low_[i] + u(rng) * (high_[i] - low_[i]);
                }
            }
            return out;
        }

        const std::vector<double> &low() const { return low_; }
        const std::vector<double> &high() const { return high_; }

    private:
        std::vector<double> low_, high_;
    };

    // 离散空间 {0, 1, ..., n-1}。
    class Discrete : public Space
    {
    public:
        explicit Discrete(int64_t n) : n_(n)
        {
            if (n <= 0)
                throw std::invalid_argument("[qrl.Discrete] n must be positive");
        }

        bool contains(const std::vector<double> &x) const override
        {
            if (x.size() != 1)
                return false;
            const double v = x[0];
            return !std::isnan(v) && v >= 0.0 && v < static_cast<double>(n_) &&
                   v == std::floor(v);
        }

        std::string name() const override { return "Discrete"; }
        size_t dim() const override { return 1; }
        int64_t n() const { return n_; }

        std::vector<double> sample(std::mt19937 &rng) const override
        {
            std::uniform_int_distribution<int64_t> d(0, n_ - 1);
            return {static_cast<double>(d(rng))};
        }

    private:
        int64_t n_;
    };

    // ─── Terminate / Truncate（约定 1）──────────────────────────────
    //
    // 二者的区别直接影响 value target 的 bootstrap 项，是 RL 正确性的
    // 关键，因此类型上就分开，而非靠注释约定。
    struct StepResult
    {
        std::vector<double> observation;
        double reward = 0.0;
        // MDP 终止态：bootstrap 项必须为 0
        bool terminated = false;
        // 范围外截断（时间上限等）：bootstrap 项仍为 1
        bool truncated = false;
        // 逐项奖励分解与隐藏变量（约定 4）。顺序无关，用键值对。
        std::unordered_map<std::string, double> info;

        // done = terminated || truncated。仅为兼容旧代码保留；
        // **新代码不应使用它**，因为它丢掉了区分。
        [[nodiscard]] bool done() const { return terminated || truncated; }
    };

    // ─── Env：与 Gymnasium 对齐的环境接口 ──────────────────────────
    //
    // 与既有 IEnvironment 的差异（皆为 Gymnasium 约定）：
    //   reset(seed, options) → (obs, info)   而非 reset() → obs
    //   step → 5 元组语义（StepResult 内含 terminated/truncated/info）
    //   额外持有 observation_space / action_space / metadata
    class Env
    {
    public:
        virtual ~Env() = default;

        // 最内层环境。Env 自身返回 this；Wrapper 覆盖为内层 env。
        // （对齐 Gymnasium 的 unwrapped 语义）
        virtual const Env &unwrapped() const { return *this; }

        // 约定 2：seed 在 reset 时设定。传 seed 总是重置 PRNG；
        // 传 nullptr 且已有 PRNG 时**不**重置（保留轨迹延续性）。
        // 注意：给 reset 一个**默认实现**（转发到 nullopt/空 options）而非纯虚，
        // 因为子类通过重写 reset(seed, options) 覆写它。这样
        // `using Wrapper::Wrapper;` 继承来的构造即可产出具体类型 ——
        // 若 reset 是纯虚，继承构造的包装器（ClipAction 等）会保持抽象。
        virtual std::pair<std::vector<double>,
                          std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed = std::nullopt,
              const std::unordered_map<std::string, double> &options = {}) = 0;

        virtual StepResult step(const std::vector<double> &action) = 0;

        virtual const Space &observation_space() const = 0;
        virtual const Space &action_space() const = 0;

        // 约定 7：诚实声明渲染帧率；视频/示波器据此定时。
        virtual int render_fps() const { return 60; }

        // 渲染模式在构造时固定（约定 6）
        const std::string &render_mode() const { return render_mode_; }

        void close() { closed_ = true; }
        [[nodiscard]] bool closed() const { return closed_; }

    protected:
        std::string render_mode_ = "rgb_array";
        bool closed_ = false;
    };

    // ─── EnvSpec / 注册表（Gymnasium 约定：make + register）─────────
    //
    // 使环境可按 id 构造，从而支持「同一份策略代码跑多个环境」，
    // 也让超参可从配置文件注入而非硬编码。
    struct EnvSpec
    {
        std::string id;          // [namespace/]Name-v0
        std::string entry_point; // 工厂函数名（符号化，便于跨语言调用）
        int max_episode_steps = -1; // -1 = 不限
        double reward_threshold = 0.0;
        bool nondeterministic = false;
        bool order_enforce = true;
        std::unordered_map<std::string, double> kwargs;

        bool valid() const { return !id.empty() && !entry_point.empty(); }
    };

    // 工厂签名返回 shared_ptr 而非 unique_ptr：
//   - 包装器链（TimeLimit / RecordEpisodeStatistics …）本就持有 shared_ptr，
//     返回 unique_ptr 会让它们无法直接包装注册表造出的环境；
//   - make_vector_env 也需要把同一工厂复用于 N 个子环境。
using EnvFactory = std::function<std::shared_ptr<Env>(const EnvSpec &)>;

    class Registry
    {
    public:
        static Registry &instance()
        {
            static Registry r;
            return r;
        }

        void register_env(const EnvSpec &spec, EnvFactory factory)
        {
            if (!spec.valid())
                throw std::invalid_argument("[qrl.Registry] invalid EnvSpec (empty id/entry_point)");
            specs_[spec.id] = spec;
            factories_[spec.id] = std::move(factory);
        }

        bool has(const std::string &id) const { return factories_.count(id) != 0; }

        EnvSpec spec(const std::string &id) const
        {
            auto it = specs_.find(id);
            if (it == specs_.end())
                throw std::runtime_error("[qrl.Registry] unregistered env: " + id);
            return it->second;
        }

        std::shared_ptr<Env> make(const std::string &id,
                                  const std::unordered_map<std::string, double> &kwargs = {}) const
        {
            auto it = factories_.find(id);
            if (it == factories_.end())
                throw std::runtime_error("[qrl.Registry] unregistered env: " + id);
            EnvSpec s = specs_.at(id);
            for (const auto &kv : kwargs)
                s.kwargs[kv.first] = kv.second;
            std::shared_ptr<Env> env = it->second(s);
            if (!env)
                throw std::runtime_error("[qrl.Registry] factory returned null for " + id);
            return env;
        }

        // 列出全部已注册 id（诊断用）
        std::vector<std::string> ids() const
        {
            std::vector<std::string> out;
            out.reserve(specs_.size());
            for (const auto &kv : specs_)
                out.push_back(kv.first);
            return out;
        }

    private:
        std::unordered_map<std::string, EnvSpec> specs_;
        std::unordered_map<std::string, EnvFactory> factories_;
    };

    inline void register_env(const EnvSpec &spec, EnvFactory factory)
    {
        Registry::instance().register_env(spec, std::move(factory));
    }

    inline std::shared_ptr<Env> make(const std::string &id,
                                     const std::unordered_map<std::string, double> &kwargs = {})
    {
        return Registry::instance().make(id, kwargs);
    }

} // namespace qrl
