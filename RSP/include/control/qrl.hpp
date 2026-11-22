#pragma once

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

    class Space
    {
    public:
        virtual ~Space() = default;
        virtual bool contains(const std::vector<double> &x) const = 0;
        virtual std::string name() const = 0;
        virtual size_t dim() const = 0;

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

    class Box : public Space
    {
    public:
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

        explicit Box(size_t dim)
            : low_(dim, -1.0), high_(dim, 1.0)
        {
            if (dim == 0)
                throw std::invalid_argument("[qrl.Box] dim must be positive");
        }

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

    struct StepResult
    {
        std::vector<double> observation;
        double reward = 0.0;
        bool terminated = false;
        bool truncated = false;
        std::unordered_map<std::string, double> info;

        [[nodiscard]] bool done() const { return terminated || truncated; }
    };

    class Env
    {
    public:
        virtual ~Env() = default;

        virtual const Env &unwrapped() const { return *this; }

        virtual std::pair<std::vector<double>,
                          std::unordered_map<std::string, double>>
        reset(std::optional<uint64_t> seed = std::nullopt,
              const std::unordered_map<std::string, double> &options = {}) = 0;

        virtual StepResult step(const std::vector<double> &action) = 0;

        virtual const Space &observation_space() const = 0;
        virtual const Space &action_space() const = 0;

        virtual int render_fps() const { return 60; }

        const std::string &render_mode() const { return render_mode_; }

        void close() { closed_ = true; }
        [[nodiscard]] bool closed() const { return closed_; }

    protected:
        std::string render_mode_ = "rgb_array";
        bool closed_ = false;
    };

    struct EnvSpec
    {
        std::string id;
        std::string entry_point;
        int max_episode_steps = -1;
        double reward_threshold = 0.0;
        bool nondeterministic = false;
        bool order_enforce = true;
        std::unordered_map<std::string, double> kwargs;

        bool valid() const { return !id.empty() && !entry_point.empty(); }
    };

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

}