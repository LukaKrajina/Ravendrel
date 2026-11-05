#include "theme_manager.h"

#include <QApplication>
#include <QSettings>

namespace rvdlrsp::gui
{
    ThemeManager &ThemeManager::instance()
    {
        static ThemeManager inst;
        return inst;
    }

    ThemeManager::ThemeManager(QObject *parent) : QObject(parent)
    {
        load_saved();
    }

    ThemeManager::Theme ThemeManager::current() const
    {
        return current_;
    }

    bool ThemeManager::isDark() const
    {
        return current_ == Theme::Dark;
    }

    void ThemeManager::setTheme(Theme t)
    {
        if (current_ == t)
            return;
        current_ = t;
        QSettings().setValue(QStringLiteral("theme"),
                            t == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
        emit themeChanged(isDark());
    }

    void ThemeManager::toggle()
    {
        setTheme(current_ == Theme::Dark ? Theme::Light : Theme::Dark);
    }

    QString ThemeManager::toggleText(Theme t)
    {
        return t == Theme::Dark ? QStringLiteral("🌙 夜间模式") : QStringLiteral("☀️ 白天模式");
    }

    ThemeManager::ViewportTheme ThemeManager::viewportTheme(bool dark)
    {
        ViewportTheme v;
        if (dark)
        {
            // 夜间：深灰蓝背景，保留原有大气散射氛围；网格线稍亮以在深色背景上清晰
            v.clear_r = 0.10f;
            v.clear_g = 0.12f;
            v.clear_b = 0.15f;
            v.sun_intensity = 22.0f;
            v.light_intensity = 1.2f;
            v.grid_tint_r = 1.35f;
            v.grid_tint_g = 1.35f;
            v.grid_tint_b = 1.35f;
        }
        else
        {
            // 白天：浅天蓝背景，更强的太阳与模型光照；网格线更暗以在浅色背景上清晰
            v.clear_r = 0.62f;
            v.clear_g = 0.78f;
            v.clear_b = 0.95f;
            v.sun_intensity = 40.0f;
            v.light_intensity = 1.6f;
            v.grid_tint_r = 0.72f;
            v.grid_tint_g = 0.72f;
            v.grid_tint_b = 0.72f;
        }
        return v;
    }

    void ThemeManager::load_saved()
    {
        const QString saved = QSettings().value(QStringLiteral("theme")).toString();
        current_ = (saved == QStringLiteral("light")) ? Theme::Light : Theme::Dark; // 默认夜间
    }

    void ThemeManager::apply(QApplication &app)
    {
        app.setStyleSheet(current_ == Theme::Dark ? darkStyleSheet() : lightStyleSheet());
    }

    QString ThemeManager::lightStyleSheet()
    {
        return QStringLiteral(R"QSS(
    /* ── 白天模式 · 全局 ─────────────────────────────────────── */
    QWidget { background: #f5f6f8; color: #1f2329; font-size: 13px; }
    QMainWindow, QDialog { background: #eef0f4; }

    QMenuBar { background: #f5f6f8; border-bottom: 1px solid #dcdfe4; }
    QMenuBar::item { padding: 5px 12px; background: transparent; border-radius: 4px; }
    QMenuBar::item:selected { background: #e2e6ec; }

    QMenu { background: #ffffff; border: 1px solid #d5d9df; }
    QMenu::item { padding: 6px 28px 6px 20px; }
    QMenu::item:selected { background: #ddeaff; color: #0b57d0; }
    QMenu::separator { height: 1px; background: #e6e8ec; margin: 4px 8px; }

    QToolBar { background: #f5f6f8; border-bottom: 1px solid #dcdfe4; spacing: 4px; padding: 3px; }
    QToolBar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 4px; }
    QToolBar QToolButton:hover { background: #e2e6ec; }
    QToolBar QToolButton:checked { background: #ddeaff; border-color: #a6c8ff; }

    QStatusBar { background: #f5f6f8; border-top: 1px solid #dcdfe4; }

    QDockWidget { titlebar-close-icon: none; }
    QDockWidget::title { background: #e8ebef; padding: 6px 10px; border: 1px solid #dcdfe4; text-align: left; }

    QTabWidget::pane { border: 1px solid #dcdfe4; top: -1px; }
    QTabBar::tab { background: #e8ebef; padding: 6px 14px; border: 1px solid #dcdfe4; border-bottom: none; }
    QTabBar::tab:selected { background: #ffffff; font-weight: bold; }

    QGroupBox { border: 1px solid #dcdfe4; border-radius: 6px; margin-top: 12px; }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }

    QTreeWidget, QListWidget { background: #ffffff; alternate-background-color: #f2f4f7; border: 1px solid #dcdfe4; border-radius: 4px; }
    QTreeWidget::item, QListWidget::item { padding: 3px 2px; }
    QTreeWidget::item:selected, QListWidget::item:selected { background: #ddeaff; color: #0b57d0; }

    QPushButton, QToolButton { background: #ffffff; border: 1px solid #c7ccd3; border-radius: 5px; padding: 5px 12px; }
    QPushButton:hover, QToolButton:hover { background: #eef3fb; border-color: #a6b3c4; }
    QPushButton:pressed, QToolButton:pressed { background: #ddeaff; }
    QPushButton:disabled, QToolButton:disabled { color: #a8adb5; background: #f0f1f3; }

    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox { background: #ffffff; border: 1px solid #c7ccd3; border-radius: 5px; padding: 4px 6px; }
    QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #a6b3c4; }
    QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #0b57d0; }
    QComboBox QAbstractItemView { background: #ffffff; border: 1px solid #d5d9df; selection-background-color: #ddeaff; selection-color: #0b57d0; }

    QPlainTextEdit, QTextEdit { background: #ffffff; color: #1f1f1f; border: 1px solid #c7ccd3; border-radius: 4px; selection-background-color: #add6ff; selection-color: #000000; }

    QCheckBox, QRadioButton { spacing: 6px; background: transparent; }

    QProgressBar { background: #e2e6ec; border: 1px solid #c7ccd3; border-radius: 5px; text-align: center; }
    QProgressBar::chunk { background: #0b57d0; border-radius: 4px; }

    QHeaderView::section { background: #e8ebef; border: 1px solid #dcdfe4; padding: 4px 6px; }
    QSplitter::handle { background: #dcdfe4; }

    QScrollBar:vertical { background: #f0f1f3; width: 12px; margin: 0; }
    QScrollBar::handle:vertical { background: #c7ccd3; border-radius: 5px; min-height: 24px; margin: 2px; }
    QScrollBar::handle:vertical:hover { background: #a6b3c4; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar:horizontal { background: #f0f1f3; height: 12px; margin: 0; }
    QScrollBar::handle:horizontal { background: #c7ccd3; border-radius: 5px; min-width: 24px; margin: 2px; }
    QScrollBar::handle:horizontal:hover { background: #a6b3c4; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

    QToolTip { background: #ffffff; color: #1f2329; border: 1px solid #c7ccd3; padding: 4px 6px; }

    /* 卡片式可选按钮（机器人类型 / 场景模板 / 工作区） */
    QPushButton[cardButton="true"] { border: 1px solid #c7ccd3; border-radius: 6px; padding: 4px; color: #3a4048; }
    QPushButton[cardButton="true"]:hover { background: #eef3fb; }
    QPushButton[cardButton="true"]:checked { border: 2px solid #0b57d0; background: #ddeaff; color: #0b57d0; font-weight: bold; }
    )QSS");
    }

    QString ThemeManager::darkStyleSheet()
    {
        return QStringLiteral(R"QSS(
    /* ── 夜间模式 · 全局 ─────────────────────────────────────── */
    QWidget { background: #1e2127; color: #d5d9e0; font-size: 13px; }
    QMainWindow, QDialog { background: #16181d; }

    QMenuBar { background: #1e2127; border-bottom: 1px solid #2a2e36; }
    QMenuBar::item { padding: 5px 12px; background: transparent; border-radius: 4px; }
    QMenuBar::item:selected { background: #2a2e36; }

    QMenu { background: #24272e; border: 1px solid #333740; }
    QMenu::item { padding: 6px 28px 6px 20px; }
    QMenu::item:selected { background: #2f3a4a; color: #8ab4ff; }
    QMenu::separator { height: 1px; background: #333740; margin: 4px 8px; }

    QToolBar { background: #1e2127; border-bottom: 1px solid #2a2e36; spacing: 4px; padding: 3px; }
    QToolBar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 4px; }
    QToolBar QToolButton:hover { background: #2a2e36; }
    QToolBar QToolButton:checked { background: #2f3a4a; border-color: #4a5160; }

    QStatusBar { background: #1e2127; border-top: 1px solid #2a2e36; }

    QDockWidget { titlebar-close-icon: none; }
    QDockWidget::title { background: #24272e; padding: 6px 10px; border: 1px solid #2a2e36; text-align: left; }

    QTabWidget::pane { border: 1px solid #2a2e36; top: -1px; }
    QTabBar::tab { background: #24272e; padding: 6px 14px; border: 1px solid #2a2e36; border-bottom: none; }
    QTabBar::tab:selected { background: #1e2127; font-weight: bold; }

    QGroupBox { border: 1px solid #2a2e36; border-radius: 6px; margin-top: 12px; }
    QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }

    QTreeWidget, QListWidget { background: #1a1c21; alternate-background-color: #202329; border: 1px solid #2a2e36; border-radius: 4px; }
    QTreeWidget::item, QListWidget::item { padding: 3px 2px; }
    QTreeWidget::item:selected, QListWidget::item:selected { background: #2f3a4a; color: #8ab4ff; }

    QPushButton, QToolButton { background: #2a2e36; border: 1px solid #3a3f49; border-radius: 5px; padding: 5px 12px; }
    QPushButton:hover, QToolButton:hover { background: #343943; border-color: #4a5160; }
    QPushButton:pressed, QToolButton:pressed { background: #2f3a4a; }
    QPushButton:disabled, QToolButton:disabled { color: #6a7180; background: #24272e; }

    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox { background: #1a1c21; border: 1px solid #3a3f49; border-radius: 5px; padding: 4px 6px; }
    QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #4a5160; }
    QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #3aa0ff; }
    QComboBox QAbstractItemView { background: #24272e; border: 1px solid #333740; selection-background-color: #2f3a4a; selection-color: #8ab4ff; }

    QPlainTextEdit, QTextEdit { background: #1e1e1e; color: #d4d4d4; border: 1px solid #3a3f49; border-radius: 4px; selection-background-color: #264f78; selection-color: #ffffff; }

    QCheckBox, QRadioButton { spacing: 6px; background: transparent; }

    QProgressBar { background: #2a2e36; border: 1px solid #3a3f49; border-radius: 5px; text-align: center; }
    QProgressBar::chunk { background: #3aa0ff; border-radius: 4px; }

    QHeaderView::section { background: #24272e; border: 1px solid #2a2e36; padding: 4px 6px; }
    QSplitter::handle { background: #2a2e36; }

    QScrollBar:vertical { background: #1a1c21; width: 12px; margin: 0; }
    QScrollBar::handle:vertical { background: #3a3f49; border-radius: 5px; min-height: 24px; margin: 2px; }
    QScrollBar::handle:vertical:hover { background: #4a5160; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar:horizontal { background: #1a1c21; height: 12px; margin: 0; }
    QScrollBar::handle:horizontal { background: #3a3f49; border-radius: 5px; min-width: 24px; margin: 2px; }
    QScrollBar::handle:horizontal:hover { background: #4a5160; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

    QToolTip { background: #24272e; color: #d5d9e0; border: 1px solid #3a3f49; padding: 4px 6px; }

    /* 卡片式可选按钮（机器人类型 / 场景模板 / 工作区） */
    QPushButton[cardButton="true"] { border: 1px solid #3a3f49; border-radius: 6px; padding: 4px; color: #d5d9e0; }
    QPushButton[cardButton="true"]:hover { background: #343943; }
    QPushButton[cardButton="true"]:checked { border: 2px solid #3aa0ff; background: #1e3a5f; color: #ffffff; font-weight: bold; }
    )QSS");
    }
}