#pragma once
#include <QDialog>

class QComboBox;

namespace rvdlrsp::gui
{
    // 应用设置对话框：统一管理主题 / 语言 / 日志级别。
    class SettingsDialog : public QDialog
    {
        Q_OBJECT
    public:
        explicit SettingsDialog(QWidget *parent = nullptr);

    signals:
        void settingsChanged(); // 用户确定后，主题/语言/日志级别已应用

    private:
        void build_ui();
        void load_current();
        void apply();

        QComboBox *theme_combo_ = nullptr;
        QComboBox *lang_combo_ = nullptr;
        QComboBox *log_combo_ = nullptr;
    };
}