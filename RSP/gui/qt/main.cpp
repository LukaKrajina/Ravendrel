#include <QApplication>
#include <QSurfaceFormat>
#include <exception>

#include "main_window.h"
#include "project_browser.h"
#include "loading_screen.h"
#include "theme_manager.h"
#include "log_manager.h"
#include "crash_handler.h"
#include "simulation_host.hpp"
#include "i18n/i18n.h"

static void msg_handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    using namespace rvdlrsp::gui;
    LogManager::Level l;
    switch (type)
    {
    case QtDebugMsg:
        l = LogManager::Level::Debug;
        break;
    case QtInfoMsg:
        l = LogManager::Level::Info;
        break;
    case QtWarningMsg:
        l = LogManager::Level::Warning;
        break;
    case QtCriticalMsg:
        l = LogManager::Level::Error;
        break;
    case QtFatalMsg:
        l = LogManager::Level::Fatal;
        break;
    default:
        l = LogManager::Level::Info;
        break;
    }
    const QString category = ctx.category ? QString::fromUtf8(ctx.category) : QStringLiteral("qt");
    LogManager::instance().write(l, category, msg);
}

#ifndef RVDLRSP_SHADER_DIR
#define RVDLRSP_SHADER_DIR shaders
#endif
#define RVDLRSP_STR_IMPL(x) #x
#define RVDLRSP_STR(x) RVDLRSP_STR_IMPL(x)

int main(int argc, char *argv[])
{
    qInstallMessageHandler(msg_handler);

    QApplication app(argc, argv);
    app.setApplicationName("Ravendrel RSP");
    app.setOrganizationName("RavendrelProject");

    rvdlrsp::gui::LogManager::instance().init();
    QKLOG_INFO("app", "=== rvdlRSP_gui start ===");

    rvdlrsp::gui::CrashHandler::install();

    rvdlrsp::gui::I18n::instance();
    rvdlrsp::gui::ThemeManager::instance().apply(app);

    try
    {
        rvdlrsp::gui::ProjectBrowser browser;
        browser.show();
        QKLOG_INFO("app", "ProjectBrowser shown");

        QObject::connect(&browser, &rvdlrsp::gui::ProjectBrowser::launchRequested,
            [&](const rvdlrsp::gui::SimulationConfig &cfg) {
                QKLOG_INFO("app", QString("launch requested: %1").arg(QString::fromStdString(cfg.project_name)));

                auto *loading = new rvdlrsp::gui::LoadingScreen(cfg);

                QObject::connect(loading, &rvdlrsp::gui::LoadingScreen::finished,
                    [&, loading](rvdlrsp::gui::SimulationHost *host) {
                        QKLOG_INFO("app", "Loading finished, creating MainWindow");
                        auto *win = new rvdlrsp::gui::MainWindow(RVDLRSP_STR(RVDLRSP_SHADER_DIR), host->config(), host);
                        win->setAttribute(Qt::WA_DeleteOnClose);
                        win->show();
                        QObject::connect(win, &QObject::destroyed, &browser,
                                         [&browser]() { browser.show(); });
                        loading->deleteLater();
                        browser.hide();
                    });

                QObject::connect(loading, &rvdlrsp::gui::LoadingScreen::failed,
                    [&, loading](const QString &step, const QString &reason) {
                        QKLOG_ERROR("app", QString("LOAD FAILED: %1 - %2").arg(step, reason));
                        loading->deleteLater();
                        browser.show();
                    });

                QObject::connect(loading, &rvdlrsp::gui::LoadingScreen::cancelled,
                    [&, loading]() {
                        QKLOG_INFO("app", "Loading cancelled");
                        loading->deleteLater();
                        browser.show();
                    });

                loading->show();
                browser.hide();
                loading->start();
            });

        return app.exec();
    }
    catch (const std::exception &e)
    {
        QKLOG_ERROR("app", QString("EXCEPTION: %1").arg(e.what()));
        return 1;
    }
    catch (...)
    {
        QKLOG_ERROR("app", "UNKNOWN EXCEPTION");
        return 2;
    }
}