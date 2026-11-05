#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace rvdlrsp::hardware
{

    // ─────────────────────────────────────────────────────────────
    // 可观测性(生产级质量)
    //
    // 结构化分级日志、指标注册表(计数器/仪表)、健康报告,
    // 为运维监控与安全审计提供基础。
    //
    // 生产化增强:
    //   - Logger 提供进程级单例 Logger::instance()
    //   - 日志带时间戳 + 分级 + 组件标签
    //   - LogStream + 宏(RVDLRSP_INFO 等)支持流式日志,
    //     与 std::cout 用法一致,最小迁移成本
    // ─────────────────────────────────────────────────────────────

    // 日志级别
    enum class LogLevel
    {
        Debug,
        Info,
        Warn,
        Error
    };

    inline const char *log_level_name(LogLevel l)
    {
        switch (l)
        {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        }
        return "UNKNOWN";
    }

    // ─── 结构化分级日志(进程级单例)─────────────────────────
    class Logger
    {
    private:
        LogLevel min_level_ = LogLevel::Info;
        std::mutex mtx_;
        uint64_t error_count_ = 0;
        uint64_t warn_count_ = 0;
        uint64_t info_count_ = 0;
        bool timestamps_ = true;

    public:
        // 进程级单例
        static Logger &instance()
        {
            static Logger l;
            return l;
        }

        void set_level(LogLevel l) { min_level_ = l; }
        LogLevel level() const { return min_level_; }
        void enable_timestamps(bool on) { timestamps_ = on; }

        void log(LogLevel level, const std::string &component, const std::string &message)
        {
            if (level < min_level_)
                return;
            std::lock_guard<std::mutex> lock(mtx_);
            switch (level)
            {
            case LogLevel::Error: ++error_count_; break;
            case LogLevel::Warn:  ++warn_count_;  break;
            case LogLevel::Info:  ++info_count_;  break;
            default: break;
            }
            if (timestamps_)
            {
                auto now = std::chrono::system_clock::now();
                std::time_t tt = std::chrono::system_clock::to_time_t(now);
                std::cerr << std::put_time(std::localtime(&tt), "%Y-%m-%d %H:%M:%S") << " ";
            }
            std::cerr << "[" << log_level_name(level) << "][" << component << "] "
                      << message << "\n";
        }

        void debug(const std::string &c, const std::string &m) { log(LogLevel::Debug, c, m); }
        void info(const std::string &c, const std::string &m)  { log(LogLevel::Info, c, m); }
        void warn(const std::string &c, const std::string &m)  { log(LogLevel::Warn, c, m); }
        void error(const std::string &c, const std::string &m) { log(LogLevel::Error, c, m); }

        uint64_t error_count() const { return error_count_; }
        uint64_t warn_count() const { return warn_count_; }
        uint64_t info_count() const { return info_count_; }
    };

    // ─── 流式日志:析构时把累积内容写入 Logger ──────────────
    // 用法:RVDLRSP_INFO("qpc") << "message " << value;
    class LogStream
    {
    private:
        LogLevel level_;
        std::string component_;
        std::ostringstream os_;

    public:
        LogStream(LogLevel level, const char *component)
            : level_(level), component_(component) {}

        LogStream(LogLevel level, std::string component)
            : level_(level), component_(std::move(component)) {}

        ~LogStream()
        {
            Logger::instance().log(level_, component_, os_.str());
        }

        LogStream(const LogStream &) = delete;
        LogStream &operator=(const LogStream &) = delete;

        template <typename T>
        LogStream &operator<<(const T &v)
        {
            os_ << v;
            return *this;
        }

        // 支持 std::endl / std::flush 等操纵符
        LogStream &operator<<(std::ostream &(*manip)(std::ostream &))
        {
            os_ << manip;
            return *this;
        }
    };

    // ─── 指标注册表(计数器 + 仪表)─────────────────────────
    class MetricsRegistry
    {
    private:
        mutable std::mutex mtx_;
        std::unordered_map<std::string, uint64_t> counters_;
        std::unordered_map<std::string, double> gauges_;

    public:
        void inc_counter(const std::string &name, uint64_t n = 1)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            counters_[name] += n;
        }

        uint64_t get_counter(const std::string &name) const
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = counters_.find(name);
            return (it == counters_.end()) ? 0 : it->second;
        }

        void set_gauge(const std::string &name, double v)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            gauges_[name] = v;
        }

        double get_gauge(const std::string &name) const
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = gauges_.find(name);
            return (it == gauges_.end()) ? 0.0 : it->second;
        }

        void reset()
        {
            std::lock_guard<std::mutex> lock(mtx_);
            counters_.clear();
            gauges_.clear();
        }
    };

    // ─── 健康状态 ────────────────────────────────────────────
    struct HealthStatus
    {
        bool healthy = true;
        std::string reason;
    };

    // ─── 健康报告:周期性检查并上报 ─────────────────────────
    class HealthReporter
    {
    private:
        MetricsRegistry *metrics_;
        std::function<bool()> check_; // 返回 true = 健康

    public:
        HealthReporter(MetricsRegistry *metrics, std::function<bool()> check)
            : metrics_(metrics), check_(std::move(check)) {}

        HealthStatus report() const
        {
            HealthStatus s;
            if (check_ && !check_())
            {
                s.healthy = false;
                s.reason = "health check failed";
            }
            return s;
        }
    };

} // namespace rvdlrsp::hardware

// ─── 流式日志宏(全项目统一入口)────────────────────────────
#define RVDLRSP_LOG(level, component) \
    ::rvdlrsp::hardware::LogStream((level), (component))

#define RVDLRSP_DEBUG(component) \
    ::rvdlrsp::hardware::LogStream(::rvdlrsp::hardware::LogLevel::Debug, (component))

#define RVDLRSP_INFO(component) \
    ::rvdlrsp::hardware::LogStream(::rvdlrsp::hardware::LogLevel::Info, (component))

#define RVDLRSP_WARN(component) \
    ::rvdlrsp::hardware::LogStream(::rvdlrsp::hardware::LogLevel::Warn, (component))

#define RVDLRSP_ERROR(component) \
    ::rvdlrsp::hardware::LogStream(::rvdlrsp::hardware::LogLevel::Error, (component))