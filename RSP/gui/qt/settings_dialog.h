#pragma once
#include <QDialog>

class QComboBox;

namespace rvdlrsp::gui
{
    class SettingsDialog : public QDialog
    {
        Q_OBJECT
    public:
        explicit SettingsDialog(QWidget *parent = nullptr);

    signals:
        void settingsChanged();

    private:
        void build_ui();
        void load_current();
        void apply();

        QComboBox *theme_combo_ = nullptr;
        QComboBox *lang_combo_ = nullptr;
        QComboBox *log_combo_ = nullptr;
    };
}