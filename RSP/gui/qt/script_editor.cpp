#include "script_editor.h"
#include "panels.h"
#include "theme_manager.h"
#include "ui_utils.h"
#include "i18n/i18n.h"

#include <QTextDocument>
#include <QFile>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QCompleter>
#include <QStringListModel>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSplitter>
#include <QDir>
#include <QCoreApplication>
#include <QTcpSocket>
#include <QMessageBox>
#include <QCloseEvent>
#include <cstdlib>

namespace rvdlrsp::gui
{

    const QStringList &rvdl_keywords()
    {
        static const QStringList kws = {
            "let", "auto", "int", "int8", "int16", "int32", "int64",
            "uint8", "uint16", "uint32", "uint64",
            "float", "double", "string", "char", "bool",
            "Qubit", "QObject", "QModel", "DiracState", "BellState", "QuantumRegister",
            "new", "return", "if", "else", "while", "for",
            "alloc", "measure", "encode_text", "encode_image", "encode_pixel", "qlm_invoke",
            "qlm_load", "rvdl_encode_string", "qlm_forward", "rvdl_decode_string",
            "mind_read", "mind_train", "mind_feedback", "veda_qlm_train",
            "mellowmax2", "logsumexp2", "boltzmann2", "tnorm_luk",
            "surrogate", "tanh_quantize", "lif_step",
            "polymer_weight", "polymer_mix_bound"};
        return kws;
    }

    QkHighlighter::QkHighlighter(QTextDocument *parent) : QSyntaxHighlighter(parent)
    {
        set_dark_mode(ThemeManager::instance().isDark());
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, &QkHighlighter::set_dark_mode);
    }

    void QkHighlighter::set_dark_mode(bool dark)
    {
        dark_ = dark;
        rebuild();
        rehighlight();
    }

    void QkHighlighter::rebuild()
    {
        rules_.clear();

        QTextCharFormat type_fmt;
        QTextCharFormat kw_fmt;
        QTextCharFormat fn_fmt;
        QTextCharFormat str_fmt;
        QTextCharFormat num_fmt;
        QTextCharFormat comment_fmt;

        if (dark_)
        {
            type_fmt.setForeground(QColor("#569cd6"));
            kw_fmt.setForeground(QColor("#c586c0"));
            fn_fmt.setForeground(QColor("#dcdcaa"));
            str_fmt.setForeground(QColor("#ce9178"));
            num_fmt.setForeground(QColor("#b5cea8"));
            comment_fmt.setForeground(QColor("#6a9955"));
        }
        else
        {
            type_fmt.setForeground(QColor("#267f99"));
            kw_fmt.setForeground(QColor("#0000ff"));
            fn_fmt.setForeground(QColor("#795e26"));
            str_fmt.setForeground(QColor("#a31515"));
            num_fmt.setForeground(QColor("#098658"));
            comment_fmt.setForeground(QColor("#008000"));
        }

        Rule type_rule;
        type_rule.pattern = QRegularExpression(
            R"(\b(int8|int16|int32|int64|uint8|uint16|uint32|uint64|float|double|string|char|bool|Qubit|QObject|QModel|DiracState|BellState|QuantumRegister)\b)");
        type_rule.format = type_fmt;
        rules_.push_back(type_rule);

        Rule kw_rule;
        kw_rule.pattern = QRegularExpression(R"(\b(let|auto|new|return|if|else|while|for|int)\b)");
        kw_rule.format = kw_fmt;
        rules_.push_back(kw_rule);

        QStringList fns;
        for (const QString &k : rvdl_keywords())
        {
            bool is_builtin =
                k == "alloc" || k == "measure" || k == "encode_text" || k == "encode_image" ||
                k == "encode_pixel" ||
                k == "qlm_invoke" || k == "qlm_load" || k == "rvdl_encode_string" ||
                k == "qlm_forward" || k == "rvdl_decode_string" || k == "mind_read" ||
                k == "mind_train" || k == "mind_feedback" || k == "veda_qlm_train" ||
                k == "mellowmax2" || k == "logsumexp2" || k == "boltzmann2" || k == "tnorm_luk" ||
                k == "surrogate" || k == "tanh_quantize" || k == "lif_step" ||
                k == "polymer_weight" || k == "polymer_mix_bound";
            if (is_builtin)
                fns << k;
        }
        Rule fn_rule;
        fn_rule.pattern = QRegularExpression(
            "\\b(" + fns.join("|") + ")\\b(?=\\s*\\()");
        fn_rule.format = fn_fmt;
        rules_.push_back(fn_rule);

        Rule str_rule;
        str_rule.pattern = QRegularExpression(R"("(?:[^"\\]|\\.)*")");
        str_rule.format = str_fmt;
        rules_.push_back(str_rule);

        Rule num_rule;
        num_rule.pattern = QRegularExpression(R"(-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)");
        num_rule.format = num_fmt;
        rules_.push_back(num_rule);

        Rule comment_rule;
        comment_rule.pattern = QRegularExpression(R"(//[^\n]*)");
        comment_rule.format = comment_fmt;
        rules_.push_back(comment_rule);
    }

    void QkHighlighter::highlightBlock(const QString &text)
    {
        for (const Rule &rule : rules_)
        {
            QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
            while (it.hasNext())
            {
                QRegularExpressionMatch m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), rule.format);
            }
        }
    }

    ScriptEditorWindow::ScriptEditorWindow(const QString &path, QWidget *parent)
        : QWidget(parent), path_(path)
    {
        setWindowTitle(RVDLTR("rvdl 脚本编辑器 — %1").arg(QFileInfo(path).fileName()));
        resize(fit_screen(0.7, QSize(900, 680)));
        setAttribute(Qt::WA_DeleteOnClose);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);

        auto *top = new QHBoxLayout();
        status_label_ = new QLabel();
        status_label_->setStyleSheet("color:#8a93a6;");
        auto *run_btn = new QPushButton(RVDLTR("▶ 运行"));
        auto *save_btn = new QPushButton(RVDLTR("保存 (Ctrl+S)"));
        auto *close_btn = new QPushButton(RVDLTR("关闭"));
        top->addWidget(status_label_, 1);
        top->addWidget(run_btn);
        top->addWidget(save_btn);
        top->addWidget(close_btn);
        lay->addLayout(top);

        auto *splitter = new QSplitter(Qt::Vertical);
        editor_ = new CodeEditor();
        editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
        splitter->addWidget(editor_);

        output_ = new QPlainTextEdit();
        output_->setReadOnly(true);
        output_->setPlaceholderText(RVDLTR("运行输出将显示在这里…"));
        splitter->addWidget(output_);
        splitter->setSizes({480, 160});
        lay->addWidget(splitter, 1);

        new QkHighlighter(editor_->document());

        auto *model = new QStringListModel(rvdl_keywords(), this);
        completer_ = new QCompleter(model, this);
        completer_->setWidget(editor_);
        completer_->setCompletionMode(QCompleter::PopupCompletion);
        completer_->setCaseSensitivity(Qt::CaseSensitive);
        completer_->setFilterMode(Qt::MatchStartsWith);
        completer_->setModelSorting(QCompleter::CaseSensitivelySortedModel);

        connect(run_btn, &QPushButton::clicked, this, &ScriptEditorWindow::run_script);
        connect(save_btn, &QPushButton::clicked, this, &ScriptEditorWindow::save_content);
        connect(close_btn, &QPushButton::clicked, this, &QWidget::close);
        connect(editor_, &QPlainTextEdit::textChanged, this, &ScriptEditorWindow::on_text_changed);

        auto *save_sc = new QShortcut(QKeySequence::Save, this);
        save_sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(save_sc, &QShortcut::activated, this, &ScriptEditorWindow::save_content);

        auto *run_sc = new QShortcut(QKeySequence(Qt::Key_F5), this);
        run_sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(run_sc, &QShortcut::activated, this, &ScriptEditorWindow::run_script);

        load_content();
        update_status();
    }

    void ScriptEditorWindow::load_content()
    {
        QFile f(path_);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            editor_->setPlainText(RVDLTR("（无法读取文件）"));
            editor_->setReadOnly(true);
            return;
        }
        original_content_ = QString::fromUtf8(f.readAll());
        QSignalBlocker b(editor_);
        editor_->setPlainText(original_content_);
        dirty_ = false;
    }

    void ScriptEditorWindow::save_content()
    {
        QFile f(path_);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        {
            QMessageBox::warning(this, RVDLTR("保存失败"),
                                 RVDLTR("无法写入文件：\n%1").arg(path_));
            return;
        }
        f.write(editor_->toPlainText().toUtf8());
        f.close();
        original_content_ = editor_->toPlainText();
        dirty_ = false;
        update_status();
        emit dirtyChanged(path_, false);
    }

    void ScriptEditorWindow::on_text_changed()
    {
        bool now = (editor_->toPlainText() != original_content_);
        if (now != dirty_)
        {
            dirty_ = now;
            update_status();
            emit dirtyChanged(path_, dirty_);
        }
    }

    void ScriptEditorWindow::update_status()
    {
        status_label_->setText(QString("%1%2")
                                   .arg(QFileInfo(path_).fileName())
                                   .arg(dirty_ ? "  " + RVDLTR("*已修改") : "  " + RVDLTR("已保存")));
    }

    void ScriptEditorWindow::closeEvent(QCloseEvent *event)
    {
        if (!dirty_)
        {
            event->accept();
            return;
        }

        const QMessageBox::StandardButton ret = QMessageBox::warning(
            this, RVDLTR("未保存的更改"),
            RVDLTR("文件 %1 有未保存的更改，是否保存？").arg(QFileInfo(path_).fileName()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (ret == QMessageBox::Save)
        {
            save_content();
            if (dirty_)
            {
                event->ignore();
                return;
            }
            event->accept();
        }
        else if (ret == QMessageBox::Discard)
        {
            event->accept();
        }
        else
        {
            event->ignore();
        }
    }

    void ScriptEditorWindow::run_script()
    {
        if (dirty_)
            save_content();

        if (!output_)
            return;
        output_->clear();
        output_->appendPlainText(RVDLTR("[运行] %1").arg(path_));
        output_->appendPlainText("------------------------------------------------");

        if (process_)
        {
            process_->kill();
            process_->deleteLater();
            process_ = nullptr;
        }

        const QString daemonHost = QString::fromLocal8Bit(
            [] { const char *e = std::getenv("RVDL_DAEMON_HOST"); return e && *e ? e : "127.0.0.1"; }());
        const quint16 daemonPort = static_cast<quint16>(
            [] { const char *e = std::getenv("RVDL_DAEMON_PORT"); return e && *e ? std::atoi(e) : 50052; }());
        auto *probe = new QTcpSocket(this);
        probe->connectToHost(daemonHost, daemonPort);
        connect(probe, &QTcpSocket::connected, this, [this, probe]()
                {
            probe->disconnectFromHost();
            probe->deleteLater();
            execute_script(); });
        connect(probe, &QAbstractSocket::errorOccurred, this, [this, probe, daemonHost, daemonPort]()
                {
            output_->appendPlainText(RVDLTR("[错误] Ravendrel Daemon 未运行（%1:%2）。")
                                         .arg(daemonHost).arg(daemonPort));
            output_->appendPlainText(RVDLTR("       请先启动: runtime --daemon"));
            output_->appendPlainText(RVDLTR("       然后再点击运行。"));
            probe->deleteLater(); });
    }

    void ScriptEditorWindow::execute_script()
    {
        process_ = new QProcess(this);

        QDir dir(QCoreApplication::applicationDirPath());
        QStringList candidates = {
            dir.absoluteFilePath("../../../server/rvdl.cmd"),
            dir.absoluteFilePath("../../../../server/rvdl.cmd"),
            dir.absoluteFilePath("server/rvdl.cmd"),
        };
        QString rvdl = "";
        for (const QString &c : candidates)
            if (QFile::exists(c))
            {
                rvdl = c;
                break;
            }

        if (rvdl.isEmpty())
        {
            output_->appendPlainText(RVDLTR("[错误] 未找到 rvdl.cmd 解释器"));
            return;
        }

        process_->setProgram("cmd");
        process_->setArguments({"/c", rvdl, "run", path_});
        process_->setWorkingDirectory(QFileInfo(rvdl).absolutePath());

        connect(process_, &QProcess::readyReadStandardOutput, this, [this]()
                {
        if (process_)
            output_->appendPlainText(QString::fromLocal8Bit(process_->readAllStandardOutput())); });
        connect(process_, &QProcess::readyReadStandardError, this, [this]()
                {
        if (process_)
            output_->appendPlainText(QString::fromLocal8Bit(process_->readAllStandardError())); });
        connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus)
                {
        output_->appendPlainText("------------------------------------------------");
        output_->appendPlainText(RVDLTR("[完成] 退出码 %1").arg(code)); });

        process_->start();
    }
}