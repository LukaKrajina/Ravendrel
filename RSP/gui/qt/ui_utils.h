#pragma once
#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <cmath>

namespace rvdlrsp::gui
{
    inline QSize fit_screen(qreal ratio, const QSize &fallback = QSize(1280, 720))
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen)
            return fallback;
        const QSize avail = screen->availableGeometry().size();
        return QSize(qRound(avail.width() * ratio), qRound(avail.height() * ratio));
    }
}