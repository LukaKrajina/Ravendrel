#include "find_replace_dialog.h"
#include "i18n/i18n.h"

#include <QPlainTextEdit>
#include <QTextDocument>
#include <QTextCursor>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>

namespace rvdlrsp::gui
{
    FindReplaceDialog::FindReplaceDialog(QPlainTextEdit *editor, QWidget *parent)
        : QDialog(parent), editor_(editor)
    {
        setWindowTitle(RVDLTR("查找/替换"));
        setWindowFlag(Qt::WindowStaysOnTopHint, true);

        auto *lay = new QVBoxLayout(this);

        // ── 查找行 ──────────────────────────────────────────────
        auto *find_row = new QHBoxLayout();
        find_edit_ = new QLineEdit(this);
        find_edit_->setPlaceholderText(RVDLTR("查找…"));
        auto *next_btn = new QPushButton(RVDLTR("下一个"), this);
        auto *prev_btn = new QPushButton(RVDLTR("上一个"), this);
        find_row->addWidget(find_edit_);
        find_row->addWidget(next_btn);
        find_row->addWidget(prev_btn);
        lay->addLayout(find_row);

        // ── 替换行（可按需隐藏）────────────────────────────────
        replace_row_ = new QWidget(this);
        auto *rep_lay = new QHBoxLayout(replace_row_);
        rep_lay->setContentsMargins(0, 0, 0, 0);
        replace_edit_ = new QLineEdit(replace_row_);
        replace_edit_->setPlaceholderText(RVDLTR("替换为…"));
        auto *replace_btn = new QPushButton(RVDLTR("替换"), replace_row_);
        auto *replace_all_btn = new QPushButton(RVDLTR("全部替换"), replace_row_);
        rep_lay->addWidget(replace_edit_);
        rep_lay->addWidget(replace_btn);
        rep_lay->addWidget(replace_all_btn);
        lay->addWidget(replace_row_);

        // ── 选项 ────────────────────────────────────────────────
        case_check_ = new QCheckBox(RVDLTR("区分大小写"), this);
        lay->addWidget(case_check_);

        connect(next_btn, &QPushButton::clicked, this, &FindReplaceDialog::find_next);
        connect(prev_btn, &QPushButton::clicked, this, &FindReplaceDialog::find_prev);
        connect(find_edit_, &QLineEdit::returnPressed, this, &FindReplaceDialog::find_next);
        connect(replace_btn, &QPushButton::clicked, this, &FindReplaceDialog::replace);
        connect(replace_all_btn, &QPushButton::clicked, this, &FindReplaceDialog::replace_all);
        connect(replace_edit_, &QLineEdit::returnPressed, this, &FindReplaceDialog::replace);

        set_replace_mode(false);
    }

    void FindReplaceDialog::set_replace_mode(bool enabled)
    {
        if (replace_row_)
            replace_row_->setVisible(enabled);
        adjustSize();
    }

    void FindReplaceDialog::focus_find()
    {
        find_edit_->setFocus();
        find_edit_->selectAll();
    }

    void FindReplaceDialog::find_next()
    {
        if (!editor_)
            return;
        const QString text = find_edit_->text();
        if (text.isEmpty())
            return;

        QTextDocument::FindFlags flags;
        if (case_check_->isChecked())
            flags |= QTextDocument::FindCaseSensitively;

        if (!editor_->find(text, flags))
        {
            // 绕到开头重找
            QTextCursor c = editor_->textCursor();
            c.movePosition(QTextCursor::Start);
            editor_->setTextCursor(c);
            editor_->find(text, flags);
        }
    }

    void FindReplaceDialog::find_prev()
    {
        if (!editor_)
            return;
        const QString text = find_edit_->text();
        if (text.isEmpty())
            return;

        QTextDocument::FindFlags flags = QTextDocument::FindBackward;
        if (case_check_->isChecked())
            flags |= QTextDocument::FindCaseSensitively;

        if (!editor_->find(text, flags))
        {
            QTextCursor c = editor_->textCursor();
            c.movePosition(QTextCursor::End);
            editor_->setTextCursor(c);
            editor_->find(text, flags);
        }
    }

    void FindReplaceDialog::replace()
    {
        if (!editor_)
            return;
        QTextCursor c = editor_->textCursor();
        if (c.hasSelection() && c.selectedText() == find_edit_->text())
            c.insertText(replace_edit_->text());
        find_next();
    }

    void FindReplaceDialog::replace_all()
    {
        if (!editor_)
            return;
        const QString find = find_edit_->text();
        if (find.isEmpty())
            return;

        QTextDocument::FindFlags flags;
        if (case_check_->isChecked())
            flags |= QTextDocument::FindCaseSensitively;

        QTextCursor c(editor_->document());
        c.movePosition(QTextCursor::Start);
        editor_->setTextCursor(c);

        int count = 0;
        while (editor_->find(find, flags))
        {
            QTextCursor cur = editor_->textCursor();
            cur.insertText(replace_edit_->text());
            ++count;
        }

        if (count == 0)
            QMessageBox::information(this, RVDLTR("全部替换"), RVDLTR("未找到匹配项。"));
    }
}