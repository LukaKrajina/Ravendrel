#pragma once
#include <QWidget>
#include <QSyntaxHighlighter>
#include <QCompleter>
#include <QVector>
#include <QString>
#include <QRegularExpression>
#include <QTextCharFormat>

class QTextDocument;
class QLabel;
class QPlainTextEdit;
class QProcess;

namespace rvdlrsp::gui
{

    class CodeEditor;

    class QkHighlighter : public QSyntaxHighlighter
    {
        Q_OBJECT
    public:
        explicit QkHighlighter(QTextDocument *parent = nullptr);

        void set_dark_mode(bool dark);

    protected:
        void highlightBlock(const QString &text) override;

    private:
        void rebuild();
        struct Rule
        {
            QRegularExpression pattern;
            QTextCharFormat format;
        };
        QVector<Rule> rules_;
        bool dark_ = true;
    };

    class ScriptEditorWindow : public QWidget
    {
        Q_OBJECT
    public:
        ScriptEditorWindow(const QString &path, QWidget *parent = nullptr);

    signals:
        void dirtyChanged(const QString &path, bool dirty);

    protected:
        void closeEvent(QCloseEvent *event) override;

    private:
        void load_content();
        void save_content();
        void on_text_changed();
        void update_status();
        void run_script();
        void execute_script();

        QString path_;
        QString original_content_;
        bool dirty_ = false;

        CodeEditor *editor_ = nullptr;
        QLabel *status_label_ = nullptr;
        QCompleter *completer_ = nullptr;
        QPlainTextEdit *output_ = nullptr;
        QProcess *process_ = nullptr;
    };

    const QStringList &rvdl_keywords();
}