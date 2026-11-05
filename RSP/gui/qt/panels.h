#pragma once
#include <string>
#include <QWidget>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QGroupBox>
#include <QCheckBox>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QVector>
#include <QColor>
#include <QFuture>

#include "simulation_config.h"

namespace rvdlrsp::gui
{

    class SimulationHost;

    // ─── 带行号栏的代码编辑器 ─────────────────────────────────────
    class CodeEditor;
    class FindReplaceDialog;

    class LineNumberArea : public QWidget
    {
    public:
        explicit LineNumberArea(CodeEditor *editor);

        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        CodeEditor *editor_;
    };

    class CodeEditor : public QPlainTextEdit
    {
        Q_OBJECT
    public:
        explicit CodeEditor(QWidget *parent = nullptr);

        void lineNumberAreaPaintEvent(QPaintEvent *event);
        int lineNumberAreaWidth() const;

        // 编辑器选项
        void set_tab_spaces(bool use_spaces); // Tab 缩进：true 用空格，false 用制表符
        void set_wrap(bool wrap);             // 自动换行
        void set_dark_mode(bool dark);        // 白天/夜间配色（自动跟随全局主题）

    protected:
        void resizeEvent(QResizeEvent *event) override;
        void keyPressEvent(QKeyEvent *event) override;
        void mousePressEvent(QMouseEvent *event) override;

    private slots:
        void updateLineNumberAreaWidth(int newBlockCount);
        void updateLineNumberArea(const QRect &rect, int dy);
        void highlightCurrentLine();

    private:
        int matchingBracketPosition(int pos) const; // 返回匹配括号位置，无则 -1
        void insert_at_all_cursors(const QString &text);
        void backspace_at_all_cursors();
        void delete_at_all_cursors();
        void clear_extra_cursors();
        void select_next_occurrence(); // Ctrl+D：选中下一个相同单词
        void goto_matching_bracket();  // 跳转到匹配括号
        void show_find(bool replace_mode); // Ctrl+F/Ctrl+H 查找替换

        // 行号栏 / 高亮等无法用 QSS 表达的配色（随主题切换）
        struct EditorColors
        {
            QColor current_line;   // 当前行背景
            QColor extra_cursor;   // 额外光标色块
            QColor bracket_bg;     // 括号匹配背景
            QColor bracket_fg;     // 括号匹配前景
            QColor ln_bg;          // 行号栏背景
            QColor ln_current_bg;  // 行号栏当前行背景
            QColor ln_fg_current;  // 当前行号前景
            QColor ln_fg;          // 普通行号前景
            QColor ln_divider;     // 行号分隔线
        };

        LineNumberArea *line_number_area_ = nullptr;
        FindReplaceDialog *find_dialog_ = nullptr; // 查找/替换（懒创建）
        bool tab_spaces_ = true;
        bool dark_ = true;
        EditorColors colors_;
        QVector<int> extra_cursors_; // 额外光标位置（文档绝对位置）
        QString search_word_;        // Ctrl+D 当前搜索的单词
        int search_from_ = -1;       // 下次搜索起始位置
    };

    // ─── JSON 语法高亮器（资产编辑器用）────────────────────────────
    class JsonHighlighter : public QSyntaxHighlighter
    {
        Q_OBJECT
    public:
        explicit JsonHighlighter(QTextDocument *parent = nullptr);

        void set_dark_mode(bool dark); // 白天/夜间语法高亮配色

    protected:
        void highlightBlock(const QString &text) override;

    private:
        void rebuild(); // 根据 dark_ 重建 rules_ 与各 format
        struct Rule
        {
            QRegularExpression pattern;
            QTextCharFormat format;
        };
        QVector<Rule> rules_;
        QTextCharFormat key_fmt_;
        QTextCharFormat string_fmt_;
        QTextCharFormat number_fmt_;
        QTextCharFormat literal_fmt_;
        bool dark_ = true;
    };

    // ─── 遥操作面板 ─────────────────────────────────────────────────
    class TeleopPanel : public QWidget
    {
        Q_OBJECT
    public:
        TeleopPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        float joint_angle() const { return joint_slider_->value() / 100.0f; }
    signals:
        void startTeleop();
        void stopTeleop();

    private:
        QSlider *joint_slider_;
        QLabel *status_;
    };

    // ─── 物理面板 ─────────────────────────────────────────────────
    class PhysicsPanel : public QWidget
    {
        Q_OBJECT
    public:
        PhysicsPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        float gravity() const { return gravity_slider_->value() / 10.0f; }
        int solver_iterations() const { return solver_spin_->value(); }
    signals:
        void pauseSim();
        void stepOnce();

    private:
        QSlider *gravity_slider_;
        QSpinBox *solver_spin_;
        QLabel *status_;
    };

    // ─── RL 面板 ──────────────────────────────────────────────────
    class RlPanel : public QWidget
    {
        Q_OBJECT
    public:
        RlPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QProgressBar *progress_;
        QLabel *reward_;
        QLabel *episode_;
        QLabel *status_;
    signals:
        void startTraining();
        void stopTraining();
    };

    // ─── 量子面板 ─────────────────────────────────────────────────
    class QuantumPanel : public QWidget
    {
        Q_OBJECT
    public:
        QuantumPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QLabel *backend_;
        QLabel *qubits_;
        QLabel *state_;
        QLabel *measure_;
    };

    // ─── 意识面板 ─────────────────────────────────────────────────
    class ConsciousnessPanel : public QWidget
    {
        Q_OBJECT
    public:
        ConsciousnessPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QProgressBar *awareness_;
        QLabel *state_;
    };

    // ─── 脑机桥面板 ───────────────────────────────────────────────
    class BrainBridgePanel : public QWidget
    {
        Q_OBJECT
    public:
        BrainBridgePanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QLabel *signal_;
        QLabel *channels_;
    };

    // ─── 电路面板 ─────────────────────────────────────────────────
    class CircuitPanel : public QWidget
    {
        Q_OBJECT
    public:
        CircuitPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QListWidget *list_;
    };

    // ─── 蓝图面板 ─────────────────────────────────────────────────
    class BlueprintPanel : public QWidget
    {
        Q_OBJECT
    public:
        BlueprintPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QListWidget *list_;
    };

    // ─── 场景图面板 ───────────────────────────────────────────────
    class SceneGraphPanel : public QWidget
    {
        Q_OBJECT
    public:
        SceneGraphPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QListWidget *list_;
    };

    // ─── 日志面板 ─────────────────────────────────────────────────
    class LogPanel : public QWidget
    {
        Q_OBJECT
    public:
        LogPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QTextEdit *text_;
    };

    // ─── 指标面板 ─────────────────────────────────────────────────
    class MetricsPanel : public QWidget
    {
        Q_OBJECT
    public:
        MetricsPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host, float fps, float frame_ms);
        QLabel *fps_;
        QLabel *frame_ms_;
        QLabel *step_;
        QLabel *norm_residual_;
    };

    // ─── 分形地形面板 ─────────────────────────────────────────────
    class FractalPanel : public QWidget
    {
        Q_OBJECT
    public:
        FractalPanel(QWidget *parent = nullptr);
        FractalTerrain params() const;
        void set_params(const FractalTerrain &f);
    signals:
        void applyRequested();

    private:
        QCheckBox *enabled_;
        QSpinBox *resolution_;
        QDoubleSpinBox *extent_;
        QDoubleSpinBox *height_scale_;
        QSpinBox *max_iter_;
        QDoubleSpinBox *slice_s_;
    };

    // ─── 实体属性面板（World Outliner 选中 → 属性编辑）────────────
    class EntityDetailsPanel : public QWidget
    {
        Q_OBJECT
    public:
        EntityDetailsPanel(QWidget *parent = nullptr);
        void show_entity(int index, const std::string &name, const std::string &kind,
                         double px, double py, double pz, double mass, const std::string &collider,
                         double rx, double ry, double rz, double sx, double sy, double sz);
        void clear_entity();
    signals:
        void positionEdited(int entity_index, double x, double y, double z);
        void rotationEdited(int entity_index, double x, double y, double z); // 欧拉角(度)
        void scaleEdited(int entity_index, double x, double y, double z);
        void deleteRequested(int entity_index); // 删除导入实体
    private:
        int entity_index_ = -1;
        QLabel *name_label_;
        QLabel *kind_label_;
        QLabel *mass_label_;
        QLabel *collider_label_;
        QDoubleSpinBox *px_, *py_, *pz_;
        QDoubleSpinBox *rx_, *ry_, *rz_;
        QDoubleSpinBox *sx_, *sy_, *sz_;
        QPushButton *delete_btn_;
    };

    // ─── 资产属性面板（Content Browser 双击资产 → 属性 + 编辑）────
    class AssetDetailsPanel : public QWidget
    {
        Q_OBJECT
    public:
        explicit AssetDetailsPanel(QWidget *parent = nullptr);

        // 打开一个资产：文本类显示可编辑内容，二进制类只读。
        void show_asset(const QString &path, const QString &type_name, int status);
        void clear_asset();

        QString current_path() const { return path_; }
        bool is_dirty() const { return dirty_; }
        void save(); // 公开保存入口（供主窗口关闭确认时调用）

    signals:
        // 内容被修改 / 保存后发出，通知内容浏览器更新 dirty 角标
        void dirtyChanged(const QString &path, bool dirty);

    private:
        void load_content();
        void save_content();
        void discard_content(); // 放弃修改，恢复磁盘内容
        void on_text_changed();
        void update_buttons();
        void update_undo_status(); // 撤销栈可视化提示

        QString path_;
        QString original_content_;
        bool dirty_ = false;

        QLabel *name_label_;
        QLabel *type_label_;
        QLabel *path_label_;
        QLabel *size_label_;
        QLabel *status_label_;
        QLabel *undo_status_;
        CodeEditor *content_edit_;
        QPushButton *save_btn_;
        QPushButton *discard_btn_;
        QPushButton *undo_btn_;
        QPushButton *redo_btn_;
        QCheckBox *tab_spaces_check_;
        QCheckBox *wrap_check_;
        JsonHighlighter *highlighter_ = nullptr;
        QFuture<void> load_future_; // 异步读文件（避免大文件阻塞 UI）
        QFuture<void> save_future_; // 异步写文件
    };
}