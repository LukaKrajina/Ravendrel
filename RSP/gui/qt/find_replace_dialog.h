#pragma once
#include <QDialog>

class QPlainTextEdit;
class QLineEdit;
class QCheckBox;
class QWidget;

namespace rvdlrsp::gui
{
    class FindReplaceDialog : public QDialog
    {
        Q_OBJECT
    public:
        explicit FindReplaceDialog(QPlainTextEdit *editor, QWidget *parent = nullptr);

        void set_replace_mode(bool enabled);
        void focus_find();

    private:
        void find_next();
        void find_prev();
        void replace();
        void replace_all();

        QPlainTextEdit *editor_ = nullptr;
        QLineEdit *find_edit_ = nullptr;
        QLineEdit *replace_edit_ = nullptr;
        QWidget *replace_row_ = nullptr;
        QCheckBox *case_check_ = nullptr;
    };
}