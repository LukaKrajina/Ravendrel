#pragma once
#include <QString>
#include <QMutex>

namespace rvdlrsp::gui
{
    // 轻量日志系统：级别过滤（Debug/Info/Warning/Error/Fatal）+ 文件超限自动轮转
    // （保留最近 N 个备份）+ 线程安全。可替换成 spdlog/Qslog 日志库。
    // QMutex 保证多线程（缩略图 worker 等）写日志安全；
    // 日志目录基于 QStandardPaths::AppDataLocation，跨平台。
    class LogManager
    {
    public:
        enum class Level
        {
            Debug = 0,
            Info,
            Warning,
            Error,
            Fatal
        };

        static LogManager &instance();

        // 初始化：创建日志目录（若不存在）。在 QApplication 之后调用。
        void init();

        void set_min_level(Level l);
        Level min_level() const { return min_level_; }

        // 测试/配置用：设置轮转阈值（字节）与备份数（默认 5MB / 3）
        void set_max_bytes(qint64 bytes);
        void set_max_backups(int n);

        // 写一条日志（线程安全）。category 用于定位模块，如 "app"/"vcs"/"thumb"。
        void write(Level l, const QString &category, const QString &msg);

        QString log_dir() const;       // 日志目录
        QString log_file_path() const; // 当前日志文件绝对路径

    private:
        LogManager();
        void rotate_if_needed();       // 文件超限 → 轮转 log.1/.2/.3
        QString level_name(Level l) const;

        QString dir_;
        QString file_path_;
        Level min_level_ = Level::Info;
        qint64 max_bytes_ = 5 * 1024 * 1024; // 5MB
        int max_backups_ = 3;
        mutable QMutex mutex_;
    };
}

// 宏（category 与 msg 均为 QString 或可隐式转换的字符串）
#define QKLOG_DEBUG(cat, msg) ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Debug, (cat), (msg))
#define QKLOG_INFO(cat, msg)  ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Info, (cat), (msg))
#define QKLOG_WARN(cat, msg)  ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Warning, (cat), (msg))
#define QKLOG_ERROR(cat, msg) ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Error, (cat), (msg))
#define QKLOG_FATAL(cat, msg) ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Fatal, (cat), (msg))