#include "panels.h"
#include "simulation_host.hpp"
#include "content_browser.h"
#include "theme_manager.h"
#include "find_replace_dialog.h"
#include "i18n/i18n.h"

#include <QString>
#include <QSignalBlocker>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QShortcut>
#include <QTextDocument>
#include <QTextBlock>
#include <QPaintEvent>
#include <QScrollBar>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPair>
#include <QtConcurrent>
#include <algorithm>

namespace rvdlrsp::gui
{

    // ─── 带行号栏的代码编辑器 ─────────────────────────────────────
    CodeEditor::CodeEditor(QWidget *parent) : QPlainTextEdit(parent)
    {
        line_number_area_ = new LineNumberArea(this);

        connect(this, &QPlainTextEdit::blockCountChanged,
                this, &CodeEditor::updateLineNumberAreaWidth);
        connect(this, &QPlainTextEdit::updateRequest,
                this, &CodeEditor::updateLineNumberArea);
        connect(this, &QPlainTextEdit::cursorPositionChanged,
                this, &CodeEditor::highlightCurrentLine);

        // 编辑器配色随全局主题（背景/前景/选区由 QSS 提供，行号/高亮由本类管理）
        set_dark_mode(ThemeManager::instance().isDark());
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, &CodeEditor::set_dark_mode);

        setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));

        updateLineNumberAreaWidth(0);
        highlightCurrentLine();
    }

    void CodeEditor::set_dark_mode(bool dark)
    {
        dark_ = dark;
        if (dark)
        {
            colors_.current_line = QColor("#2a2a2a");
            colors_.extra_cursor = QColor("#3aa0ff");
            colors_.bracket_bg = QColor("#5a4a1a");
            colors_.bracket_fg = QColor("#ffffff");
            colors_.ln_bg = QColor("#1e1e1e");
            colors_.ln_current_bg = QColor("#2a2a2a");
            colors_.ln_fg_current = QColor("#c8c8c8");
            colors_.ln_fg = QColor("#6a6a6a");
            colors_.ln_divider = QColor("#3a3a3a");
        }
        else
        {
            colors_.current_line = QColor("#f5f5f5");
            colors_.extra_cursor = QColor("#007acc");
            colors_.bracket_bg = QColor("#ffff99");
            colors_.bracket_fg = QColor("#000000");
            colors_.ln_bg = QColor("#f5f5f5");
            colors_.ln_current_bg = QColor("#e0e0e0");
            colors_.ln_fg_current = QColor("#1f1f1f");
            colors_.ln_fg = QColor("#8a8a8a");
            colors_.ln_divider = QColor("#d0d0d0");
        }
        highlightCurrentLine();
        if (line_number_area_)
            line_number_area_->update();
    }

    void CodeEditor::set_tab_spaces(bool use_spaces)
    {
        tab_spaces_ = use_spaces;
    }

    void CodeEditor::set_wrap(bool wrap)
    {
        setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    }

    void CodeEditor::keyPressEvent(QKeyEvent *event)
    {
        // Ctrl+F / Ctrl+H：查找 / 替换
        if (event->modifiers() & Qt::ControlModifier)
        {
            if (event->key() == Qt::Key_F)
            {
                show_find(false);
                return;
            }
            if (event->key() == Qt::Key_H)
            {
                show_find(true);
                return;
            }
        }

        // Ctrl+D：选中下一个相同单词（多光标）
        if (event->key() == Qt::Key_D && (event->modifiers() & Qt::ControlModifier))
        {
            select_next_occurrence();
            return;
        }

        // Ctrl+M 或 Ctrl+Shift+\：跳转到匹配括号
        if ((event->modifiers() & Qt::ControlModifier) &&
            (event->key() == Qt::Key_M ||
             (event->key() == Qt::Key_Backslash && (event->modifiers() & Qt::ShiftModifier))))
        {
            goto_matching_bracket();
            return;
        }

        // Esc 清除额外光标
        if (event->key() == Qt::Key_Escape && !extra_cursors_.isEmpty())
        {
            clear_extra_cursors();
            return;
        }

        // Tab 缩进：按 Tab 插入 4 空格（或制表符）；Shift+Tab 反向缩进
        if (event->key() == Qt::Key_Tab && tab_spaces_)
        {
            QTextCursor cursor = textCursor();
            if (event->modifiers() & Qt::ShiftModifier)
            {
                // 反向缩进：删除行首最多 4 个空格
                cursor.beginEditBlock();
                cursor.movePosition(QTextCursor::StartOfLine);
                int removed = 0;
                while (removed < 4)
                {
                    QTextCursor peek = cursor;
                    if (peek.atBlockEnd())
                        break;
                    QChar ch = document()->characterAt(cursor.position());
                    if (ch == ' ')
                    {
                        cursor.deleteChar();
                        ++removed;
                    }
                    else
                    {
                        break;
                    }
                }
                cursor.endEditBlock();
            }
            else
            {
                insertPlainText("    ");
            }
            return;
        }

        // ── 多光标输入 ──────────────────────────────────────────
        if (!extra_cursors_.isEmpty())
        {
            const QString text = event->text();
            if (!text.isEmpty() && text != "\r" && text != "\n" && text != "\t")
            {
                insert_at_all_cursors(text);
                return;
            }
            if (event->key() == Qt::Key_Backspace)
            {
                backspace_at_all_cursors();
                return;
            }
            if (event->key() == Qt::Key_Delete)
            {
                delete_at_all_cursors();
                return;
            }
            if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            {
                insert_at_all_cursors("\n");
                return;
            }
        }

        QPlainTextEdit::keyPressEvent(event);
    }

    void CodeEditor::mousePressEvent(QMouseEvent *event)
    {
        // Alt+左键 → 添加额外光标
        if (event->modifiers() & Qt::AltModifier)
        {
            QTextCursor cursor = cursorForPosition(event->position().toPoint());
            if (!cursor.isNull())
            {
                int pos = cursor.position();
                if (!extra_cursors_.contains(pos))
                    extra_cursors_.append(pos);
                highlightCurrentLine();
                event->accept();
                return;
            }
        }
        // 普通点击清除额外光标
        clear_extra_cursors();
        QPlainTextEdit::mousePressEvent(event);
    }

    void CodeEditor::clear_extra_cursors()
    {
        if (!extra_cursors_.isEmpty() || !search_word_.isEmpty())
        {
            extra_cursors_.clear();
            search_word_.clear();
            search_from_ = -1;
            highlightCurrentLine();
        }
    }

    void CodeEditor::insert_at_all_cursors(const QString &text)
    {
        // 收集所有光标位置（主光标 + 额外光标），去重
        QVector<int> positions = extra_cursors_;
        positions.append(textCursor().position());
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

        QTextCursor c = textCursor();
        c.beginEditBlock();
        // 从后往前插入，避免位置偏移影响
        for (int i = positions.size() - 1; i >= 0; --i)
        {
            QTextCursor ins(document());
            ins.setPosition(positions[i]);
            ins.insertText(text);
        }
        c.endEditBlock();

        // 更新额外光标位置（每个位置插入 text.length() 个字符后整体右移）
        const int delta = text.length();
        for (int &p : extra_cursors_)
        {
            // 主光标位置已由 insertText 推进；额外光标统一右移
            int base = p;
            // 找出有多少个额外光标位置 <= p（含自己），这些位置都在 p 之前或等于 p 插入了文本
            int insert_before = 0;
            for (int q : positions)
                if (q <= p)
                    ++insert_before;
            p = base + delta * insert_before;
        }
        setTextCursor(c);
        highlightCurrentLine();
    }

    void CodeEditor::backspace_at_all_cursors()
    {
        QVector<int> positions = extra_cursors_;
        positions.append(textCursor().position());
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

        QTextCursor c = textCursor();
        c.beginEditBlock();
        for (int i = positions.size() - 1; i >= 0; --i)
        {
            if (positions[i] <= 0)
                continue;
            QTextCursor del(document());
            del.setPosition(positions[i]);
            del.deletePreviousChar();
        }
        c.endEditBlock();

        // 每个被删除的位置及其右侧整体左移 1
        for (int &p : extra_cursors_)
        {
            int removed_before = 0;
            for (int q : positions)
                if (q > 0 && q <= p)
                    ++removed_before;
            p = std::max(0, p - removed_before);
        }
        setTextCursor(c);
        highlightCurrentLine();
    }

    void CodeEditor::delete_at_all_cursors()
    {
        QVector<int> positions = extra_cursors_;
        positions.append(textCursor().position());
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

        QTextCursor c = textCursor();
        c.beginEditBlock();
        for (int i = positions.size() - 1; i >= 0; --i)
        {
            QTextCursor del(document());
            del.setPosition(positions[i]);
            if (!del.atBlockEnd())
                del.deleteChar();
        }
        c.endEditBlock();

        // Delete 只删除右侧字符，位置不变（额外光标位置无需调整）
        setTextCursor(c);
        highlightCurrentLine();
    }

    void CodeEditor::highlightCurrentLine()
    {
        QList<QTextEdit::ExtraSelection> extraSelections;

        // 当前行高亮
        if (!isReadOnly())
        {
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(colors_.current_line);
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sel.cursor = textCursor();
            sel.cursor.clearSelection();
            extraSelections.append(sel);
        }

        // 额外光标高亮（竖线样式，用窄条表示）
        QTextCursor cur = textCursor();
        for (int pos : extra_cursors_)
        {
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(colors_.extra_cursor);
            sel.cursor = QTextCursor(document());
            sel.cursor.setPosition(pos);
            // 选中一个字符以显示光标色块（若在文末则选中前一个字符）
            if (pos < document()->characterCount() - 1)
                sel.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
            else if (pos > 0)
            {
                sel.cursor.setPosition(pos - 1);
                sel.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
            }
            extraSelections.append(sel);
        }

        // 括号匹配高亮
        int pos = cur.position();
        // 检查光标左侧或右侧的括号
        int bracket_pos = -1;
        if (pos > 0)
        {
            QChar left = document()->characterAt(pos - 1);
            if (left == '(' || left == ')' || left == '[' || left == ']' ||
                left == '{' || left == '}')
                bracket_pos = pos - 1;
        }
        if (bracket_pos < 0)
        {
            QChar right = document()->characterAt(pos);
            if (right == '(' || right == ')' || right == '[' || right == ']' ||
                right == '{' || right == '}')
                bracket_pos = pos;
        }
        if (bracket_pos >= 0)
        {
            int match = matchingBracketPosition(bracket_pos);
            if (match >= 0)
            {
                for (int p : {bracket_pos, match})
                {
                    QTextEdit::ExtraSelection sel;
                    sel.format.setBackground(colors_.bracket_bg);
                    sel.format.setForeground(colors_.bracket_fg);
                    sel.cursor = QTextCursor(document());
                    sel.cursor.setPosition(p);
                    sel.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
                    extraSelections.append(sel);
                }
            }
        }

        setExtraSelections(extraSelections);
    }

    int CodeEditor::matchingBracketPosition(int pos) const
    {
        const QString text = document()->toPlainText();
        if (pos < 0 || pos >= text.length())
            return -1;

        const QChar ch = text.at(pos);
        QChar open, close;
        int dir;
        if (ch == '(') { open = '('; close = ')'; dir = +1; }
        else if (ch == '[') { open = '['; close = ']'; dir = +1; }
        else if (ch == '{') { open = '{'; close = '}'; dir = +1; }
        else if (ch == ')') { open = '('; close = ')'; dir = -1; }
        else if (ch == ']') { open = '['; close = ']'; dir = -1; }
        else if (ch == '}') { open = '{'; close = '}'; dir = -1; }
        else return -1;

        int depth = 0;
        for (int i = pos; i >= 0 && i < text.length(); i += dir)
        {
            const QChar c = text.at(i);
            if (c == open) ++depth;
            else if (c == close) --depth;
            if (depth == 0 && i != pos)
                return i;
        }
        return -1;
    }

    void CodeEditor::select_next_occurrence()
    {
        const QString text = document()->toPlainText();
        QTextCursor cur = textCursor();

        // 首次：取光标下单词（或已有选区）
        if (search_word_.isEmpty())
        {
            if (cur.hasSelection())
            {
                search_word_ = cur.selectedText();
                search_from_ = cur.selectionEnd();
            }
            else
            {
                QTextCursor w = cur;
                w.select(QTextCursor::WordUnderCursor);
                search_word_ = w.selectedText();
                search_from_ = w.selectionEnd();
            }
            if (search_word_.isEmpty())
                return;
            // 把当前单词位置作为第一个额外光标
            int first = cur.hasSelection() ? cur.selectionStart() : [&]() {
                QTextCursor w = cur;
                w.select(QTextCursor::WordUnderCursor);
                return w.selectionStart();
            }();
            if (!extra_cursors_.contains(first))
                extra_cursors_.append(first);
        }

        // 从上次位置向后找下一个相同单词
        int start = (search_from_ >= 0) ? search_from_ : cur.selectionEnd();
        int found = text.indexOf(search_word_, start, Qt::CaseSensitive);
        if (found < 0 && start > 0)
            found = text.indexOf(search_word_, 0, Qt::CaseSensitive);  // 循环回开头
        if (found < 0)
            return;

        if (!extra_cursors_.contains(found))
            extra_cursors_.append(found);
        search_from_ = found + search_word_.length();
        highlightCurrentLine();
    }

    void CodeEditor::goto_matching_bracket()
    {
        QTextCursor cur = textCursor();
        int pos = cur.position();
        int bracket = -1;
        if (pos > 0)
        {
            QChar c = document()->characterAt(pos - 1);
            if (QString("()[]{}").contains(c))
                bracket = pos - 1;
        }
        if (bracket < 0)
        {
            QChar c = document()->characterAt(pos);
            if (QString("()[]{}").contains(c))
                bracket = pos;
        }
        if (bracket < 0)
            return;

        int match = matchingBracketPosition(bracket);
        if (match < 0)
            return;
        QTextCursor nc(document());
        nc.setPosition(match + 1);
        setTextCursor(nc);
        centerCursor();
        highlightCurrentLine();
    }

    void CodeEditor::show_find(bool replace_mode)
    {
        if (!find_dialog_)
            find_dialog_ = new FindReplaceDialog(this, this); // 随 CodeEditor 析构
        find_dialog_->set_replace_mode(replace_mode);
        find_dialog_->show();
        find_dialog_->raise();
        find_dialog_->activateWindow();
        find_dialog_->focus_find();
    }

    int CodeEditor::lineNumberAreaWidth() const
    {
        int digits = 1;
        int max = qMax(1, blockCount());
        while (max >= 10) { max /= 10; ++digits; }
        int space = 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
        return space;
    }

    void CodeEditor::resizeEvent(QResizeEvent *event)
    {
        QPlainTextEdit::resizeEvent(event);
        QRect cr = contentsRect();
        line_number_area_->setGeometry(
            QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    }

    void CodeEditor::updateLineNumberAreaWidth(int /*newBlockCount*/)
    {
        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    }

    void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
    {
        if (dy)
            line_number_area_->scroll(0, dy);
        else
            line_number_area_->update(0, rect.y(), line_number_area_->width(), rect.height());

        if (rect.contains(viewport()->rect()))
            updateLineNumberAreaWidth(0);
    }

    void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
    {
        QPainter painter(line_number_area_);
        painter.fillRect(event->rect(), colors_.ln_bg);

        // 当前行号（高亮）
        QTextBlock current_block = textCursor().block();
        const int current_line = current_block.blockNumber();

        QTextBlock block = firstVisibleBlock();
        int block_number = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        int bottom = top + qRound(blockBoundingRect(block).height());
        const int width = line_number_area_->width();

        while (block.isValid() && top <= event->rect().bottom())
        {
            if (block.isVisible() && bottom >= event->rect().top())
            {
                // 当前行背景条
                if (block_number == current_line)
                {
                    painter.fillRect(0, top, width, bottom - top, colors_.ln_current_bg);
                }

                QString number = QString::number(block_number + 1);
                if (block_number == current_line)
                    painter.setPen(colors_.ln_fg_current);   // 当前行号更亮
                else
                    painter.setPen(colors_.ln_fg);   // 其余行号更暗
                painter.drawText(0, top, width - 6, fontMetrics().height(),
                                 Qt::AlignRight, number);
            }
            block = block.next();
            top = bottom;
            bottom = top + qRound(blockBoundingRect(block).height());
            ++block_number;
        }

        // 右侧分隔线
        painter.setPen(colors_.ln_divider);
        painter.drawLine(width - 1, event->rect().top(), width - 1, event->rect().bottom());
    }

    LineNumberArea::LineNumberArea(CodeEditor *editor)
        : QWidget(editor), editor_(editor) {}

    QSize LineNumberArea::sizeHint() const
    {
        return QSize(editor_->lineNumberAreaWidth(), 0);
    }

    void LineNumberArea::paintEvent(QPaintEvent *event)
    {
        editor_->lineNumberAreaPaintEvent(event);
    }

    // ─── JSON 语法高亮器 ───────────────────────────────────────────
    JsonHighlighter::JsonHighlighter(QTextDocument *parent) : QSyntaxHighlighter(parent)
    {
        set_dark_mode(ThemeManager::instance().isDark());
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, &JsonHighlighter::set_dark_mode);
    }

    void JsonHighlighter::set_dark_mode(bool dark)
    {
        dark_ = dark;
        rebuild();
        rehighlight();
    }

    void JsonHighlighter::rebuild()
    {
        rules_.clear();

        if (dark_)
        {
            key_fmt_.setForeground(QColor("#9cdcfe"));     // 键名
            string_fmt_.setForeground(QColor("#ce9178")); // 字符串值
            number_fmt_.setForeground(QColor("#b5cea8")); // 数字
            literal_fmt_.setForeground(QColor("#569cd6")); // true/false/null
        }
        else
        {
            key_fmt_.setForeground(QColor("#0451a5"));
            string_fmt_.setForeground(QColor("#a31515"));
            number_fmt_.setForeground(QColor("#098658"));
            literal_fmt_.setForeground(QColor("#0000ff"));
        }
        key_fmt_.setFontWeight(QFont::Bold);

        Rule key_rule;
        key_rule.pattern = QRegularExpression(R"("(?:[^"\\]|\\.)*"(?=\s*:))");
        key_rule.format = key_fmt_;
        rules_.push_back(key_rule);

        Rule string_rule;
        string_rule.pattern = QRegularExpression(R"("(?:[^"\\]|\\.)*")");
        string_rule.format = string_fmt_;
        rules_.push_back(string_rule);

        Rule number_rule;
        number_rule.pattern = QRegularExpression(R"(-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)");
        number_rule.format = number_fmt_;
        rules_.push_back(number_rule);

        Rule literal_rule;
        literal_rule.pattern = QRegularExpression(R"(\b(?:true|false|null)\b)");
        literal_rule.format = literal_fmt_;
        rules_.push_back(literal_rule);
    }

    void JsonHighlighter::highlightBlock(const QString &text)
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

    // ─── 遥操作 ─────────────────────────────────────────────────────
    TeleopPanel::TeleopPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("遥操作 Teleop"), this);
        auto *gl = new QVBoxLayout(g);

        joint_slider_ = new QSlider(Qt::Horizontal, g);
        joint_slider_->setRange(-314, 314);  // -3.14 ~ 3.14 rad（×100）
        gl->addWidget(new QLabel(RVDLTR("关节角 (rad):"), g));
        gl->addWidget(joint_slider_);

        status_ = new QLabel(RVDLTR("空闲"), g);
        gl->addWidget(status_);

        auto *btn_row = new QHBoxLayout();
        auto *start = new QPushButton(RVDLTR("启动遥操作"), g);
        auto *stop = new QPushButton(RVDLTR("停止"), g);
        btn_row->addWidget(start);
        btn_row->addWidget(stop);
        gl->addLayout(btn_row);
        connect(start, &QPushButton::clicked, this, [this]() { emit startTeleop(); });
        connect(stop, &QPushButton::clicked, this, [this]() { emit stopTeleop(); });

        lay->addWidget(g);
    }

    void TeleopPanel::refresh(SimulationHost &host)
    {
        joint_slider_->setValue(static_cast<int>(host.joint_angle() * 100.0f));
        status_->setText(QString::fromStdString(host.teleop_status()));
    }

    // ─── 物理 ─────────────────────────────────────────────────────
    PhysicsPanel::PhysicsPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("物理引擎 Physics"), this);
        auto *gl = new QVBoxLayout(g);

        gl->addWidget(new QLabel(RVDLTR("重力 g (m/s²):"), g));
        gravity_slider_ = new QSlider(Qt::Horizontal, g);
        gravity_slider_->setRange(0, 300);  // 0 ~ 30.0（×10）
        gl->addWidget(gravity_slider_);

        gl->addWidget(new QLabel(RVDLTR("求解迭代次数:"), g));
        solver_spin_ = new QSpinBox(g);
        solver_spin_->setRange(1, 32);
        gl->addWidget(solver_spin_);

        status_ = new QLabel(RVDLTR("运行中"), g);
        gl->addWidget(status_);

        auto *btn_row = new QHBoxLayout();
        auto *pause = new QPushButton(RVDLTR("暂停"), g);
        auto *step = new QPushButton(RVDLTR("单步"), g);
        btn_row->addWidget(pause);
        btn_row->addWidget(step);
        gl->addLayout(btn_row);
        connect(pause, &QPushButton::clicked, this, [this]() { emit pauseSim(); });
        connect(step, &QPushButton::clicked, this, [this]() { emit stepOnce(); });

        lay->addWidget(g);
    }

    void PhysicsPanel::refresh(SimulationHost &host)
    {
        gravity_slider_->setValue(static_cast<int>(host.gravity() * 10.0f));
        solver_spin_->setValue(host.solver_iterations());
        status_->setText(QString::fromStdString(host.physics_status()));
    }

    // ─── RL ────────────────────────────────────────────────────────
    RlPanel::RlPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("强化学习 RL"), this);
        auto *gl = new QVBoxLayout(g);

        progress_ = new QProgressBar(g);
        progress_->setRange(0, 100);
        gl->addWidget(progress_);

        reward_ = new QLabel(RVDLTR("Reward: %1").arg(0), g);
        episode_ = new QLabel(RVDLTR("Episode: %1").arg(0), g);
        status_ = new QLabel(RVDLTR("空闲"), g);
        gl->addWidget(reward_);
        gl->addWidget(episode_);
        gl->addWidget(status_);

        auto *btn_row = new QHBoxLayout();
        auto *start = new QPushButton(RVDLTR("开始训练"), g);
        auto *stop = new QPushButton(RVDLTR("停止"), g);
        btn_row->addWidget(start);
        btn_row->addWidget(stop);
        gl->addLayout(btn_row);
        connect(start, &QPushButton::clicked, this, [this]() { emit startTraining(); });
        connect(stop, &QPushButton::clicked, this, [this]() { emit stopTraining(); });

        lay->addWidget(g);
    }

    void RlPanel::refresh(SimulationHost &host)
    {
        progress_->setValue(static_cast<int>(host.rl_progress() * 100.0f));
        reward_->setText(RVDLTR("Reward: %1").arg(host.rl_reward(), 0, 'f', 3));
        episode_->setText(RVDLTR("Episode: %1").arg(host.rl_episode()));
        status_->setText(QString::fromStdString(host.rl_status()));
    }

    // ─── 量子 ─────────────────────────────────────────────────────
    QuantumPanel::QuantumPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("量子后端 Quantum"), this);
        auto *gl = new QVBoxLayout(g);
        backend_ = new QLabel(g);
        qubits_ = new QLabel(g);
        state_ = new QLabel(g);
        measure_ = new QLabel(g);
        gl->addWidget(backend_);
        gl->addWidget(qubits_);
        gl->addWidget(state_);
        gl->addWidget(measure_);
        lay->addWidget(g);
    }

    void QuantumPanel::refresh(SimulationHost &host)
    {
        backend_->setText(QString::fromStdString(host.quantum_backend()));
        qubits_->setText(RVDLTR("量子比特数: %1").arg(host.quantum_num_qubits()));
        state_->setText(QString::fromStdString(host.quantum_state()));
        measure_->setText(QString::fromStdString(host.quantum_last_measure()));
    }

    // ─── 意识 ─────────────────────────────────────────────────────
    ConsciousnessPanel::ConsciousnessPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("意识控制器 Consciousness"), this);
        auto *gl = new QVBoxLayout(g);
        awareness_ = new QProgressBar(g);
        awareness_->setRange(0, 100);
        state_ = new QLabel(g);
        gl->addWidget(awareness_);
        gl->addWidget(state_);
        lay->addWidget(g);
    }

    void ConsciousnessPanel::refresh(SimulationHost &host)
    {
        awareness_->setValue(static_cast<int>(host.consciousness_awareness() * 100.0f));
        state_->setText(QString::fromStdString(host.consciousness_state()));
    }

    // ─── 脑机桥 ───────────────────────────────────────────────────
    BrainBridgePanel::BrainBridgePanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("脑机桥 BrainBridge"), this);
        auto *gl = new QVBoxLayout(g);
        signal_ = new QLabel(g);
        channels_ = new QLabel(g);
        gl->addWidget(signal_);
        gl->addWidget(channels_);
        lay->addWidget(g);
    }

    void BrainBridgePanel::refresh(SimulationHost &host)
    {
        signal_->setText(QString::fromStdString(host.brain_signal()));
        channels_->setText(QString::fromStdString(host.brain_channel_state()));
    }

    // ─── 电路 ─────────────────────────────────────────────────────
    CircuitPanel::CircuitPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("机器人电路 Circuit"), this);
        auto *gl = new QVBoxLayout(g);
        list_ = new QListWidget(g);
        gl->addWidget(list_);
        lay->addWidget(g);
    }

    void CircuitPanel::refresh(SimulationHost &host)
    {
        const auto &items = host.circuit_gates();
        if (list_->count() != static_cast<int>(items.size()))
        {
            list_->clear();
            for (const auto &s : items)
                list_->addItem(QString::fromStdString(s));
        }
    }

    // ─── 蓝图 ─────────────────────────────────────────────────────
    BlueprintPanel::BlueprintPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("蓝图 Blueprint"), this);
        auto *gl = new QVBoxLayout(g);
        list_ = new QListWidget(g);
        gl->addWidget(list_);
        lay->addWidget(g);
    }

    void BlueprintPanel::refresh(SimulationHost &host)
    {
        const auto &items = host.blueprint_nodes();
        if (list_->count() != static_cast<int>(items.size()))
        {
            list_->clear();
            for (const auto &s : items)
                list_->addItem(QString::fromStdString(s));
        }
    }

    // ─── 场景图 ───────────────────────────────────────────────────
    SceneGraphPanel::SceneGraphPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("场景图 Scene Graph"), this);
        auto *gl = new QVBoxLayout(g);
        list_ = new QListWidget(g);
        gl->addWidget(list_);
        lay->addWidget(g);
    }

    void SceneGraphPanel::refresh(SimulationHost &host)
    {
        const auto &items = host.scene_items();
        list_->clear();
        for (const auto &s : items)
            list_->addItem(QString::fromStdString(s));
    }

    // ─── 日志 ─────────────────────────────────────────────────────
    LogPanel::LogPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("日志 Log"), this);
        auto *gl = new QVBoxLayout(g);
        text_ = new QTextEdit(g);
        text_->setReadOnly(true);
        gl->addWidget(text_);
        lay->addWidget(g);
    }

    void LogPanel::refresh(SimulationHost &host)
    {
        text_->setPlainText(QString::fromStdString(host.log_text()));
    }

    // ─── 指标 ─────────────────────────────────────────────────────
    MetricsPanel::MetricsPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("指标 Metrics"), this);
        auto *gl = new QVBoxLayout(g);
        fps_ = new QLabel(g);
        frame_ms_ = new QLabel(g);
        step_ = new QLabel(g);
        norm_residual_ = new QLabel(g);
        gl->addWidget(fps_);
        gl->addWidget(frame_ms_);
        gl->addWidget(step_);
        gl->addWidget(norm_residual_);
        lay->addWidget(g);
    }

    void MetricsPanel::refresh(SimulationHost &host, float fps, float frame_ms)
    {
        fps_->setText(RVDLTR("FPS: %1").arg(fps, 0, 'f', 1));
        frame_ms_->setText(RVDLTR("帧时间: %1 ms").arg(frame_ms, 0, 'f', 2));
        step_->setText(RVDLTR("仿真步数: %1").arg(host.sim_step()));
        norm_residual_->setText(RVDLTR("态矢量残差 |1-‖ψ‖|: %1").arg(host.quantum_normalization_residual(), 0, 'e', 2));
    }

    // ─── 分形地形面板 ─────────────────────────────────────────────
    FractalPanel::FractalPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("分形地形 Fractal Terrain"), this);
        auto *gl = new QVBoxLayout(g);

        enabled_ = new QCheckBox(RVDLTR("启用分形地形"), g);
        gl->addWidget(enabled_);

        gl->addWidget(new QLabel(RVDLTR("分辨率（每边采样点）:"), g));
        resolution_ = new QSpinBox(g);
        resolution_->setRange(16, 512);
        resolution_->setSingleStep(16);
        gl->addWidget(resolution_);

        gl->addWidget(new QLabel(RVDLTR("范围 extent（截面 [-e, e]²）:"), g));
        extent_ = new QDoubleSpinBox(g);
        extent_->setRange(0.5, 20.0);
        extent_->setDecimals(2);
        extent_->setSingleStep(0.5);
        gl->addWidget(extent_);

        gl->addWidget(new QLabel(RVDLTR("高度系数 height_scale:"), g));
        height_scale_ = new QDoubleSpinBox(g);
        height_scale_->setRange(0.001, 1.0);
        height_scale_->setDecimals(3);
        height_scale_->setSingleStep(0.005);
        gl->addWidget(height_scale_);

        gl->addWidget(new QLabel(RVDLTR("最大迭代 max_iter:"), g));
        max_iter_ = new QSpinBox(g);
        max_iter_->setRange(16, 1024);
        max_iter_->setSingleStep(16);
        gl->addWidget(max_iter_);

        gl->addWidget(new QLabel(RVDLTR("固定 s 分量 slice_s:"), g));
        slice_s_ = new QDoubleSpinBox(g);
        slice_s_->setRange(-3.0, 3.0);
        slice_s_->setDecimals(3);
        slice_s_->setSingleStep(0.1);
        gl->addWidget(slice_s_);

        auto *apply = new QPushButton(RVDLTR("应用并重新生成"), g);
        gl->addWidget(apply);
        connect(apply, &QPushButton::clicked, this, [this]() { emit applyRequested(); });

        lay->addWidget(g);
    }

    FractalTerrain FractalPanel::params() const
    {
        FractalTerrain f;
        f.enabled = enabled_->isChecked();
        f.resolution = resolution_->value();
        f.extent = extent_->value();
        f.height_scale = height_scale_->value();
        f.max_iter = max_iter_->value();
        f.slice_s = slice_s_->value();
        return f;
    }

    void FractalPanel::set_params(const FractalTerrain &f)
    {
        enabled_->setChecked(f.enabled);
        resolution_->setValue(f.resolution);
        extent_->setValue(f.extent);
        height_scale_->setValue(f.height_scale);
        max_iter_->setValue(f.max_iter);
        slice_s_->setValue(f.slice_s);
    }

    // ─── 实体属性面板（Outliner 选中 → 属性编辑）──────────────────
    EntityDetailsPanel::EntityDetailsPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("实体属性 Entity Details"), this);
        auto *gl = new QVBoxLayout(g);

        name_label_ = new QLabel(RVDLTR("未选中实体"), g);
        kind_label_ = new QLabel(g);
        mass_label_ = new QLabel(g);
        collider_label_ = new QLabel(g);
        gl->addWidget(name_label_);
        gl->addWidget(kind_label_);
        gl->addWidget(mass_label_);
        gl->addWidget(collider_label_);

        gl->addWidget(new QLabel(RVDLTR("位置 Position (m):"), g));
        auto *pos_row = new QHBoxLayout();
        px_ = new QDoubleSpinBox(g);
        py_ = new QDoubleSpinBox(g);
        pz_ = new QDoubleSpinBox(g);
        for (auto *s : {px_, py_, pz_})
        {
            s->setRange(-1000.0, 1000.0);
            s->setDecimals(3);
            s->setSingleStep(0.1);
            pos_row->addWidget(s);
        }
        gl->addLayout(pos_row);

        gl->addWidget(new QLabel(RVDLTR("旋转 Rotation (°):"), g));
        auto *rot_row = new QHBoxLayout();
        rx_ = new QDoubleSpinBox(g);
        ry_ = new QDoubleSpinBox(g);
        rz_ = new QDoubleSpinBox(g);
        for (auto *s : {rx_, ry_, rz_})
        {
            s->setRange(-180.0, 180.0);
            s->setDecimals(1);
            s->setSingleStep(1.0);
            rot_row->addWidget(s);
        }
        gl->addLayout(rot_row);

        gl->addWidget(new QLabel(RVDLTR("缩放 Scale:"), g));
        auto *scl_row = new QHBoxLayout();
        sx_ = new QDoubleSpinBox(g);
        sy_ = new QDoubleSpinBox(g);
        sz_ = new QDoubleSpinBox(g);
        for (auto *s : {sx_, sy_, sz_})
        {
            s->setRange(0.01, 100.0);
            s->setDecimals(2);
            s->setSingleStep(0.1);
            s->setValue(1.0);
            scl_row->addWidget(s);
        }
        gl->addLayout(scl_row);

        delete_btn_ = new QPushButton(RVDLTR("删除实体"), g);
        delete_btn_->setVisible(false);
        gl->addWidget(delete_btn_);

        lay->addWidget(g);

        auto emit_pos = [this]() {
            if (entity_index_ >= 0)
                emit positionEdited(entity_index_, px_->value(), py_->value(), pz_->value());
        };
        auto emit_rot = [this]() {
            if (entity_index_ >= 0)
                emit rotationEdited(entity_index_, rx_->value(), ry_->value(), rz_->value());
        };
        auto emit_scl = [this]() {
            if (entity_index_ >= 0)
                emit scaleEdited(entity_index_, sx_->value(), sy_->value(), sz_->value());
        };
        connect(px_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_pos](double) { emit_pos(); });
        connect(py_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_pos](double) { emit_pos(); });
        connect(pz_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_pos](double) { emit_pos(); });
        connect(rx_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_rot](double) { emit_rot(); });
        connect(ry_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_rot](double) { emit_rot(); });
        connect(rz_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_rot](double) { emit_rot(); });
        connect(sx_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_scl](double) { emit_scl(); });
        connect(sy_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_scl](double) { emit_scl(); });
        connect(sz_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_scl](double) { emit_scl(); });
        connect(delete_btn_, &QPushButton::clicked, this, [this]() {
            if (entity_index_ >= 0)
                emit deleteRequested(entity_index_);
        });
    }

    void EntityDetailsPanel::show_entity(int index, const std::string &name, const std::string &kind,
                                         double px, double py, double pz, double mass, const std::string &collider,
                                         double rx, double ry, double rz, double sx, double sy, double sz)
    {
        entity_index_ = index;
        name_label_->setText(QString::fromStdString(name));
        kind_label_->setText(RVDLTR("类型: %1").arg(QString::fromStdString(kind)));
        mass_label_->setText(RVDLTR("质量: %1 kg").arg(mass, 0, 'f', 2));
        collider_label_->setText(RVDLTR("碰撞体: %1").arg(QString::fromStdString(collider)));

        // 仅导入的外部模型可删除，且可编辑旋转/缩放
        const bool imported = (kind == "imported");
        delete_btn_->setVisible(imported);
        rx_->setEnabled(imported);
        ry_->setEnabled(imported);
        rz_->setEnabled(imported);
        sx_->setEnabled(imported);
        sy_->setEnabled(imported);
        sz_->setEnabled(imported);

        // 用户正在拖动编辑时，不被物理每帧刷新覆盖
        if (!px_->hasFocus() && !py_->hasFocus() && !pz_->hasFocus())
        {
            QSignalBlocker b1(px_), b2(py_), b3(pz_);
            px_->setValue(px);
            py_->setValue(py);
            pz_->setValue(pz);
        }
        if (!rx_->hasFocus() && !ry_->hasFocus() && !rz_->hasFocus())
        {
            QSignalBlocker b1(rx_), b2(ry_), b3(rz_);
            rx_->setValue(rx);
            ry_->setValue(ry);
            rz_->setValue(rz);
        }
        if (!sx_->hasFocus() && !sy_->hasFocus() && !sz_->hasFocus())
        {
            QSignalBlocker b1(sx_), b2(sy_), b3(sz_);
            sx_->setValue(sx);
            sy_->setValue(sy);
            sz_->setValue(sz);
        }
    }

    void EntityDetailsPanel::clear_entity()
    {
        entity_index_ = -1;
        name_label_->setText(RVDLTR("未选中实体"));
        kind_label_->setText("");
        mass_label_->setText("");
        collider_label_->setText("");
        delete_btn_->setVisible(false);
        QSignalBlocker b1(px_), b2(py_), b3(pz_);
        px_->setValue(0.0);
        py_->setValue(0.0);
        pz_->setValue(0.0);
        QSignalBlocker b4(rx_), b5(ry_), b6(rz_);
        rx_->setValue(0.0);
        ry_->setValue(0.0);
        rz_->setValue(0.0);
        QSignalBlocker b7(sx_), b8(sy_), b9(sz_);
        sx_->setValue(1.0);
        sy_->setValue(1.0);
        sz_->setValue(1.0);
    }

    // ─── 资产属性面板（Content Browser 双击 → 属性 + 编辑）────────
    AssetDetailsPanel::AssetDetailsPanel(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        auto *g = new QGroupBox(RVDLTR("资产属性 Asset Details"), this);
        auto *gl = new QVBoxLayout(g);

        name_label_ = new QLabel(RVDLTR("未选中资产"), g);
        QFont nf = name_label_->font();
        nf.setBold(true);
        nf.setPixelSize(14);
        name_label_->setFont(nf);
        gl->addWidget(name_label_);

        type_label_ = new QLabel(g);
        path_label_ = new QLabel(g);
        path_label_->setWordWrap(true);
        size_label_ = new QLabel(g);
        status_label_ = new QLabel(g);
        gl->addWidget(type_label_);
        gl->addWidget(path_label_);
        gl->addWidget(size_label_);
        gl->addWidget(status_label_);

        auto *btn_row = new QHBoxLayout();
        save_btn_ = new QPushButton(RVDLTR("保存"), g);
        save_btn_->setEnabled(false);
        discard_btn_ = new QPushButton(RVDLTR("放弃修改"), g);
        discard_btn_->setEnabled(false);
        undo_btn_ = new QPushButton(RVDLTR("撤销"), g);
        undo_btn_->setEnabled(false);
        redo_btn_ = new QPushButton(RVDLTR("重做"), g);
        redo_btn_->setEnabled(false);
        btn_row->addWidget(save_btn_);
        btn_row->addWidget(discard_btn_);
        btn_row->addWidget(undo_btn_);
        btn_row->addWidget(redo_btn_);
        gl->addLayout(btn_row);

        undo_status_ = new QLabel(RVDLTR("撤销栈: %1  重做栈: %2").arg(0).arg(0), g);
        gl->addWidget(undo_status_);

        auto *opt_row = new QHBoxLayout();
        tab_spaces_check_ = new QCheckBox(RVDLTR("Tab 缩进(空格)"), g);
        tab_spaces_check_->setChecked(true);
        wrap_check_ = new QCheckBox(RVDLTR("自动换行"), g);
        wrap_check_->setChecked(false);
        opt_row->addWidget(tab_spaces_check_);
        opt_row->addWidget(wrap_check_);
        opt_row->addStretch(1);
        gl->addLayout(opt_row);

        content_edit_ = new CodeEditor(g);
        content_edit_->setReadOnly(true);
        gl->addWidget(content_edit_, 1);

        lay->addWidget(g);

        connect(tab_spaces_check_, &QCheckBox::toggled, content_edit_, &CodeEditor::set_tab_spaces);
        connect(wrap_check_, &QCheckBox::toggled, content_edit_, &CodeEditor::set_wrap);

        connect(save_btn_, &QPushButton::clicked, this, &AssetDetailsPanel::save_content);
        connect(discard_btn_, &QPushButton::clicked, this, &AssetDetailsPanel::discard_content);
        connect(content_edit_, &QPlainTextEdit::textChanged, this, &AssetDetailsPanel::on_text_changed);
        connect(undo_btn_, &QPushButton::clicked, content_edit_, &QPlainTextEdit::undo);
        connect(redo_btn_, &QPushButton::clicked, content_edit_, &QPlainTextEdit::redo);
        connect(content_edit_, &QPlainTextEdit::undoAvailable, this, &AssetDetailsPanel::update_undo_status);
        connect(content_edit_, &QPlainTextEdit::redoAvailable, this, &AssetDetailsPanel::update_undo_status);

        // Ctrl+S 保存（作用于本面板及其子控件）
        auto *save_sc = new QShortcut(QKeySequence::Save, this);
        save_sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(save_sc, &QShortcut::activated, this, &AssetDetailsPanel::save_content);

        // Ctrl+Shift+Z 放弃修改（恢复磁盘内容）
        auto *discard_sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), this);
        discard_sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(discard_sc, &QShortcut::activated, this, &AssetDetailsPanel::discard_content);
    }

    void AssetDetailsPanel::show_asset(const QString &path, const QString &type_name, int status)
    {
        path_ = path;
        dirty_ = false;
        original_content_.clear();

        QFileInfo fi(path);
        name_label_->setText(fi.fileName());
        type_label_->setText(RVDLTR("类型: %1").arg(type_name));
        path_label_->setText(RVDLTR("路径: %1").arg(fi.absoluteFilePath()));
        size_label_->setText(RVDLTR("大小: %1 字节").arg(fi.size()));
        size_label_->setText(RVDLTR("大小: %1 字节   修改时间: %2")
            .arg(fi.size())
            .arg(fi.lastModified().toString("yyyy-MM-dd hh:mm:ss")));

        QString status_text;
        if (status & AssetStatus::Dirty)      status_text += RVDLTR("*已修改") + " ";
        if (status & AssetStatus::CheckedOut) status_text += RVDLTR("✓已检出") + " ";
        if (status & AssetStatus::Added)      status_text += RVDLTR("+新增") + " ";
        if (status_text.isEmpty())            status_text = RVDLTR("干净");
        status_label_->setText(RVDLTR("状态: %1").arg(status_text));

        // 文本类资产 → 可编辑；二进制类 → 只读
        bool editable = asset_is_text_editable(path);
        if (editable) {
            load_content();
            content_edit_->setReadOnly(false);
        } else {
            content_edit_->setReadOnly(true);
            content_edit_->setPlainText(RVDLTR("（二进制资产，不支持文本编辑）"));
        }
        update_buttons();

        // JSON 资产 → 启用语法高亮；否则移除高亮器（恢复纯文本）
        if (asset_is_json(path)) {
            if (!highlighter_)
                highlighter_ = new JsonHighlighter(content_edit_->document());
        } else if (highlighter_) {
            delete highlighter_;
            highlighter_ = nullptr;
        }
    }

    void AssetDetailsPanel::clear_asset()
    {
        path_.clear();
        dirty_ = false;
        original_content_.clear();
        name_label_->setText(RVDLTR("未选中资产"));
        type_label_->clear();
        path_label_->clear();
        size_label_->clear();
        status_label_->clear();
        QSignalBlocker b(content_edit_);
        content_edit_->clear();
        content_edit_->setReadOnly(true);
        update_buttons();
    }

    void AssetDetailsPanel::load_content()
    {
        const QString path = path_;
        // 异步读文件，避免大文件阻塞 UI 线程；回主线程后检查是否仍是当前资产
        load_future_ = QtConcurrent::run([path]() -> QPair<bool, QString> {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                return {false, QString()};
            return {true, QString::fromUtf8(f.readAll())};
        }).then(this, [this, path](const QPair<bool, QString> &result) {
            if (path_ != path)
                return; // 用户已切换到其他资产，丢弃过期结果
            if (result.first) {
                original_content_ = result.second;
                QSignalBlocker b(content_edit_);
                content_edit_->setPlainText(original_content_);
                dirty_ = false;
            } else {
                content_edit_->setPlainText(RVDLTR("（无法读取文件）"));
                content_edit_->setReadOnly(true);
            }
        });
    }

    void AssetDetailsPanel::save()
    {
        // 同步保存：供 MainWindow::closeEvent 关闭确认时立即获得结果
        if (path_.isEmpty())
            return;
        const QString path = path_;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        {
            QMessageBox::warning(this, RVDLTR("保存失败"),
                                 RVDLTR("无法写入文件：\n%1").arg(path));
            return;
        }
        f.write(content_edit_->toPlainText().toUtf8());
        f.close();
        original_content_ = content_edit_->toPlainText();
        dirty_ = false;
        update_buttons();
        emit dirtyChanged(path_, false);
    }

    void AssetDetailsPanel::save_content()
    {
        if (path_.isEmpty())
            return;
        const QString path = path_;
        const QString content = content_edit_->toPlainText();
        // 异步写文件，避免大文件（大 JSON）阻塞 UI 线程
        save_future_ = QtConcurrent::run([path, content]() -> bool {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return false;
            const bool ok = (f.write(content.toUtf8()) >= 0);
            f.close();
            return ok;
        }).then(this, [this, path, content](bool ok) {
            if (path_ != path)
                return; // 用户已切换到其他资产，丢弃过期结果
            if (!ok)
            {
                QMessageBox::warning(this, RVDLTR("保存失败"),
                                     RVDLTR("无法写入文件：\n%1").arg(path));
                return;
            }
            original_content_ = content;
            dirty_ = false;
            update_buttons();
            emit dirtyChanged(path_, false);
        });
    }

    void AssetDetailsPanel::discard_content()
    {
        if (path_.isEmpty())
            return;
        // 恢复磁盘内容
        load_content();
        dirty_ = false;
        update_buttons();
        emit dirtyChanged(path_, false);
    }

    void AssetDetailsPanel::on_text_changed()
    {
        if (path_.isEmpty() || !asset_is_text_editable(path_))
            return;
        bool now_dirty = (content_edit_->toPlainText() != original_content_);
        if (now_dirty != dirty_) {
            dirty_ = now_dirty;
            update_buttons();
            emit dirtyChanged(path_, dirty_);
        }
    }

    void AssetDetailsPanel::update_buttons()
    {
        bool editable = !path_.isEmpty() && asset_is_text_editable(path_);
        save_btn_->setEnabled(editable && dirty_);
        discard_btn_->setEnabled(editable && dirty_);
    }

    void AssetDetailsPanel::update_undo_status()
    {
        if (!content_edit_)
            return;
        int undo_steps = content_edit_->document()->availableUndoSteps();
        int redo_steps = content_edit_->document()->availableRedoSteps();
        undo_status_->setText(RVDLTR("撤销栈: %1  重做栈: %2").arg(undo_steps).arg(redo_steps));
        undo_btn_->setEnabled(undo_steps > 0);
        redo_btn_->setEnabled(redo_steps > 0);
    }
}