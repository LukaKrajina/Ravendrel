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

        void set_tab_spaces(bool use_spaces);
        void set_wrap(bool wrap);
        void set_dark_mode(bool dark);

    protected:
        void resizeEvent(QResizeEvent *event) override;
        void keyPressEvent(QKeyEvent *event) override;
        void mousePressEvent(QMouseEvent *event) override;

    private slots:
        void updateLineNumberAreaWidth(int newBlockCount);
        void updateLineNumberArea(const QRect &rect, int dy);
        void highlightCurrentLine();

    private:
        int matchingBracketPosition(int pos) const;
        void insert_at_all_cursors(const QString &text);
        void backspace_at_all_cursors();
        void delete_at_all_cursors();
        void clear_extra_cursors();
        void select_next_occurrence();
        void goto_matching_bracket();
        void show_find(bool replace_mode);

        struct EditorColors
        {
            QColor current_line;
            QColor extra_cursor;
            QColor bracket_bg;
            QColor bracket_fg;
            QColor ln_bg;
            QColor ln_current_bg;
            QColor ln_fg_current;
            QColor ln_fg;
            QColor ln_divider;
        };

        LineNumberArea *line_number_area_ = nullptr;
        FindReplaceDialog *find_dialog_ = nullptr;
        bool tab_spaces_ = true;
        bool dark_ = true;
        EditorColors colors_;
        QVector<int> extra_cursors_;
        QString search_word_;
        int search_from_ = -1;
    };

    class JsonHighlighter : public QSyntaxHighlighter
    {
        Q_OBJECT
    public:
        explicit JsonHighlighter(QTextDocument *parent = nullptr);

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
        QTextCharFormat key_fmt_;
        QTextCharFormat string_fmt_;
        QTextCharFormat number_fmt_;
        QTextCharFormat literal_fmt_;
        bool dark_ = true;
    };

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

    class ConsciousnessPanel : public QWidget
    {
        Q_OBJECT
    public:
        ConsciousnessPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QProgressBar *awareness_;
        QLabel *state_;
    };

    class BrainBridgePanel : public QWidget
    {
        Q_OBJECT
    public:
        BrainBridgePanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QLabel *signal_;
        QLabel *channels_;
    };

    class CircuitPanel : public QWidget
    {
        Q_OBJECT
    public:
        CircuitPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QListWidget *list_;
    };

    class BlueprintPanel : public QWidget
    {
        Q_OBJECT
    public:
        BlueprintPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QListWidget *list_;
    };

    class SceneGraphPanel : public QWidget
    {
        Q_OBJECT
    public:
        SceneGraphPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QListWidget *list_;
    };

    class LogPanel : public QWidget
    {
        Q_OBJECT
    public:
        LogPanel(QWidget *parent = nullptr);
        void refresh(SimulationHost &host);
        QTextEdit *text_;
    };

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
        void rotationEdited(int entity_index, double x, double y, double z);
        void scaleEdited(int entity_index, double x, double y, double z);
        void deleteRequested(int entity_index);
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

    class AssetDetailsPanel : public QWidget
    {
        Q_OBJECT
    public:
        explicit AssetDetailsPanel(QWidget *parent = nullptr);

        void show_asset(const QString &path, const QString &type_name, int status);
        void clear_asset();

        QString current_path() const { return path_; }
        bool is_dirty() const { return dirty_; }
        void save();

    signals:
        void dirtyChanged(const QString &path, bool dirty);

    private:
        void load_content();
        void save_content();
        void discard_content();
        void on_text_changed();
        void update_buttons();
        void update_undo_status();

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
        QFuture<void> load_future_;
        QFuture<void> save_future_;
    };
}