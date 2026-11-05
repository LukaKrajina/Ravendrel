#pragma once
#include <QObject>
#include <QString>

class QApplication;

namespace rvdlrsp::gui
{
    // 白天 / 夜间主题管理器
    //
    // 通过全局 QSS 统一控制所有 Qt 窗口外观（工程浏览器 / 主面 / 加载界面 /
    // 脚本编辑器 / 资产查看器等独立窗口都会自动继承）。
    // 用 QSettings 持久化用户选择，默认夜间模式。
    // 提供 3D 视口所需的背景色 / 太阳强度 / 模型光照参数，供 QVulkanViewport
    // 在切换主题时联动调整（纯 C++ 传值，无需改动 shader）。
    // 主题变化时发射 themeChanged，供编辑器等组件（CodeEditor/语法高亮器）
    // 自动跟随切换配色。
    class ThemeManager : public QObject
    {
        Q_OBJECT
    public:
        enum class Theme { Light, Dark };

        // 3D 视口光照参数（白天 / 夜间）
        struct ViewportTheme
        {
            float clear_r = 0.10f;      // render pass 背景色
            float clear_g = 0.12f;
            float clear_b = 0.15f;
            float sun_intensity = 22.0f;  // 大气散射 + 太阳盘强度
            float light_intensity = 1.2f; // 模型 PBR 直接光强度
            float grid_tint_r = 1.0f;     // 网格线颜色调制系数
            float grid_tint_g = 1.0f;
            float grid_tint_b = 1.0f;
        };

        static ThemeManager &instance();

        Theme current() const;             // 当前主题
        bool isDark() const;               // 是否夜间模式
        void setTheme(Theme t);            // 切换并持久化（并发射 themeChanged）
        void toggle();                     // 白天 <-> 夜间

        void apply(QApplication &app);     // 将当前主题 QSS 应用到整个应用

        static QString toggleText(Theme t);          // 按钮/菜单文案，如 "🌙 夜间模式"
        static ViewportTheme viewportTheme(bool dark); // 3D 视口背景/光照参数

    signals:
        void themeChanged(bool dark);      // 主题已切换（编辑器等组件据此刷新配色）

    private:
        explicit ThemeManager(QObject *parent = nullptr);
        static QString lightStyleSheet();
        static QString darkStyleSheet();
        void load_saved();

        Theme current_ = Theme::Dark;
    };
}