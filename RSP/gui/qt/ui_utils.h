#pragma once
#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <cmath>

namespace rvdlrsp::gui
{
    // 根据屏幕可用尺寸计算窗口初始大小（高 DPI / 多分辨率适配）。
    // 固定像素尺寸在小屏（1366x768）会超出、在 4K 会偏小；改为按屏幕可用区域
    // 的比例计算，保证在不同分辨率 / 缩放比下都有合理初始窗口。
    // ratio 为占屏幕可用区域的比例（0~1）；无屏幕时回退 fallback。
    inline QSize fit_screen(qreal ratio, const QSize &fallback = QSize(1280, 720))
    {
        const QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen)
            return fallback;
        const QSize avail = screen->availableGeometry().size();
        return QSize(qRound(avail.width() * ratio), qRound(avail.height() * ratio));
    }
}