#include "crash_handler.h"
#include "log_manager.h"

#include <QStandardPaths>
#include <QDir>
#include <QByteArray>

#include <csignal>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <cstdio>
#include <ctime>
#include <execinfo.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace rvdlrsp::gui
{

    #if defined(__unix__) || defined(__APPLE__)

    namespace
    {
        // 崩溃日志路径（install 时预获取，信号处理器中只读，避免调用非安全函数）
        char g_crash_log_path[1024] = {0};

        void crash_signal_handler(int sig)
        {
            const char *name = "UNKNOWN";
            switch (sig)
            {
            case SIGSEGV:
                name = "SIGSEGV";
                break;
            case SIGABRT:
                name = "SIGABRT";
                break;
            case SIGFPE:
                name = "SIGFPE";
                break;
            case SIGBUS:
                name = "SIGBUS";
                break;
            case SIGILL:
                name = "SIGILL";
                break;
            }

            // 仅使用 async-signal-safe 调用：open/write/backtrace/backtrace_symbols_fd/close
            if (g_crash_log_path[0] != '\0')
            {
                int fd = open(g_crash_log_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
                if (fd >= 0)
                {
                    char header[256];
                    int hl = snprintf(header, sizeof(header),
                                    "\n=== Fatal signal %s (%d) at %lld ===\n",
                                    name, sig, static_cast<long long>(time(nullptr)));
                    write(fd, header, static_cast<size_t>(hl));

                    void *frames[64];
                    const int n = backtrace(frames, 64);
                    backtrace_symbols_fd(frames, n, fd);
                    close(fd);
                }
            }

            // 恢复默认处理并重新抛出，生成 core dump 供事后分析
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    #endif // __unix__ || __APPLE__

    void CrashHandler::install()
    {
        static bool installed = false;
        if (installed)
            return;
        installed = true;

    #if defined(__unix__) || defined(__APPLE__)
        // 预获取崩溃日志路径（避免信号处理器中调用 QStandardPaths/QDir 等非安全函数）
        QString dir = LogManager::instance().log_dir();
        if (dir.isEmpty())
        {
            QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            if (base.isEmpty())
                base = QDir::homePath() + QStringLiteral("/.rvdlrsp");
            dir = base + QStringLiteral("/logs");
        }
        QDir().mkpath(dir);
        const QByteArray bytes = (dir + QStringLiteral("/crash.log")).toLocal8Bit();
        strncpy(g_crash_log_path, bytes.constData(), sizeof(g_crash_log_path) - 1);

        signal(SIGSEGV, crash_signal_handler);
        signal(SIGABRT, crash_signal_handler);
        signal(SIGFPE, crash_signal_handler);
        signal(SIGBUS, crash_signal_handler);
        signal(SIGILL, crash_signal_handler);
    #else
        // Windows：完整崩溃捕获需 MiniDumpWriteDump（见 crashpad/breakpad），此处预留
    #endif
    }
}