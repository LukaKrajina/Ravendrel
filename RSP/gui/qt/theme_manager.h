#pragma once
#include <QObject>
#include <QString>

class QApplication;

namespace rvdlrsp::gui
{
    class ThemeManager : public QObject
    {
        Q_OBJECT
    public:
        enum class Theme { Light, Dark };

        struct ViewportTheme
        {
            float clear_r = 0.10f;
            float clear_g = 0.12f;
            float clear_b = 0.15f;
            float sun_intensity = 22.0f;
            float light_intensity = 1.2f;
            float grid_tint_r = 1.0f;
            float grid_tint_g = 1.0f;
            float grid_tint_b = 1.0f;
        };

        static ThemeManager &instance();

        Theme current() const;
        bool isDark() const;
        void setTheme(Theme t);
        void toggle();

        void apply(QApplication &app);

        static QString toggleText(Theme t);
        static ViewportTheme viewportTheme(bool dark);

    signals:
        void themeChanged(bool dark);

    private:
        explicit ThemeManager(QObject *parent = nullptr);
        static QString lightStyleSheet();
        static QString darkStyleSheet();
        void load_saved();

        Theme current_ = Theme::Dark;
    };
}