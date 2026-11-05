#pragma once
#include <QDialog>

class QPlainTextEdit;
class QLineEdit;
class QCheckBox;
class QWidget;

namespace rvdlrsp::gui
{
    // 查找/替换对话框（非模态，作用于目标编辑器）。
    class FindReplaceDialog : public QDialog
    {
        Q_OBJECT
    public:
        explicit FindReplaceDialog(QPlainTextEdit *editor, QWidget *parent = nullptr);

        void set_replace_mode(bool enabled); // 是否显示替换行
        void focus_find();                   // 聚焦并全选查找框

    private:
        void find_next();
        void find_prev();
        void replace();
        void replace_all();

        QPlainTextEdit *editor_ = nullptr;
        QLineEdit *find_edit_ = nullptr;
        QLineEdit *replace_edit_ = nullptr;
        QWidget *replace_row_ = nullptr; // 替换行容器
        QCheckBox *case_check_ = nullptr;
    };
}