#pragma once
#include <QString>
#include <QMutex>

namespace rvdlrsp::gui
{
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

        void init();

        void set_min_level(Level l);
        Level min_level() const { return min_level_; }

        void set_max_bytes(qint64 bytes);
        void set_max_backups(int n);

        void write(Level l, const QString &category, const QString &msg);

        QString log_dir() const;
        QString log_file_path() const;

    private:
        LogManager();
        void rotate_if_needed();
        QString level_name(Level l) const;

        QString dir_;
        QString file_path_;
        Level min_level_ = Level::Info;
        qint64 max_bytes_ = 5 * 1024 * 1024;
        int max_backups_ = 3;
        mutable QMutex mutex_;
    };
}

#define QKLOG_DEBUG(cat, msg) ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Debug, (cat), (msg))
#define QKLOG_INFO(cat, msg)  ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Info, (cat), (msg))
#define QKLOG_WARN(cat, msg)  ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Warning, (cat), (msg))
#define QKLOG_ERROR(cat, msg) ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Error, (cat), (msg))
#define QKLOG_FATAL(cat, msg) ::rvdlrsp::gui::LogManager::instance().write(::rvdlrsp::gui::LogManager::Level::Fatal, (cat), (msg))