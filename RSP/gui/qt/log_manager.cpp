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
            return;

        rotate_if_needed();

        QFile f(file_path_);
        if (!f.open(QIODevice::Append | QIODevice::Text))
            return;

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

        QFile::remove(file_path_ + QStringLiteral(".") + QString::number(max_backups_));
        for (int i = max_backups_ - 1; i >= 1; --i)
        {
            const QString src = file_path_ + QStringLiteral(".") + QString::number(i);
            const QString dst = file_path_ + QStringLiteral(".") + QString::number(i + 1);
            if (QFile::exists(src))
                QFile::rename(src, dst);
        }
        QFile::rename(file_path_, file_path_ + QStringLiteral(".1"));
    }
}