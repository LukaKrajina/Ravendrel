#include "log_manager.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QMutexLocker>

namespace rvdlrsp::gui
{
    LogManager &LogManager::instance()
    {
        static LogManager inst;
        return inst;
    }

    LogManager::LogManager()
    {
        // AppDataLocation 依赖 QApplication 的 applicationName/organizationName，
        // 在 LogManager 构造时可能尚未设置，所以延迟到 init() 里再解析路径。
    }

    void LogManager::init()
    {
        QMutexLocker lock(&mutex_);
        if (dir_.isEmpty())
        {
            QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            if (base.isEmpty())
                base = QDir::homePath() + "/.rvdlrsp";
            dir_ = base + "/logs";
            file_path_ = dir_ + "/rvdlrsp_gui.log";
        }
        QDir().mkpath(dir_);
    }

    void LogManager::set_min_level(Level l)
    {
        QMutexLocker lock(&mutex_);
        min_level_ = l;
    }

    void LogManager::set_max_bytes(qint64 bytes)
    {
        QMutexLocker lock(&mutex_);
        max_bytes_ = bytes;
    }

    void LogManager::set_max_backups(int n)
    {
        QMutexLocker lock(&mutex_);
        max_backups_ = n;
    }

    QString LogManager::log_dir() const
    {
        QMutexLocker lock(&mutex_);
        return dir_;
    }

    QString LogManager::log_file_path() const
    {
        QMutexLocker lock(&mutex_);
        return file_path_;
    }

    QString LogManager::level_name(Level l) const
    {
        switch (l)
        {
        case Level::Debug:
            return QStringLiteral("DEBUG");
        case Level::Info:
            return QStringLiteral("INFO");
        case Level::Warning:
            return QStringLiteral("WARN");
        case Level::Error:
            return QStringLiteral("ERROR");
        case Level::Fatal:
            return QStringLiteral("FATAL");
        }
        return QStringLiteral("INFO");
    }

    void LogManager::write(Level l, const QString &category, const QString &msg)
    {
        if (l < min_level_)
            return;

        QMutexLocker lock(&mutex_);

        if (file_path_.isEmpty())
            return; // 未 init（或 QApplication 尚未就绪），静默丢弃

        rotate_if_needed();

        QFile f(file_path_);
        if (!f.open(QIODevice::Append | QIODevice::Text))
            return; // 日志写失败静默（避免日志系统自身递归报错）

        const QString line = QStringLiteral("%1 [%2] [%3] %4\n")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")),
                                    level_name(l),
                                    category,
                                    msg);
        f.write(line.toUtf8());
        f.close();
    }

    void LogManager::rotate_if_needed()
    {
        QFileInfo fi(file_path_);
        if (!fi.exists() || fi.size() < max_bytes_)
            return;

        // 删除最旧备份 log.N
        QFile::remove(file_path_ + QStringLiteral(".") + QString::number(max_backups_));
        // 依次后移 log.(N-1) → log.N ... log.1 → log.2
        for (int i = max_backups_ - 1; i >= 1; --i)
        {
            const QString src = file_path_ + QStringLiteral(".") + QString::number(i);
            const QString dst = file_path_ + QStringLiteral(".") + QString::number(i + 1);
            if (QFile::exists(src))
                QFile::rename(src, dst);
        }
        // 当前文件 → log.1（下次 write 会重新创建空文件）
        QFile::rename(file_path_, file_path_ + QStringLiteral(".1"));
    }
}