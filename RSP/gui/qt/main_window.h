#pragma once
#include <QMainWindow>
#include <QTimer>
#include <QVulkanInstance>
#include <QTreeWidget>
#include <memory>

#include "panels.h"
#include "simulation_config.h"
#include "editor/plugin_manager.hpp"

class QTabWidget;
class QMenu;
class QAction;
class QToolBar;
class QToolButton;
class QActionGroup;

namespace rvdlrsp::gui
{

    class SimulationHost;
    class QVulkanViewport;
    class ContentBrowser;
    class FractalPanel;

    // 实体变换模式（视口工具栏）
    enum class TransformMode
    {
        Translate, // 平移
        Rotate,    // 旋转
        Scale,     // 缩放
    };

    class MainWindow : public QMainWindow
    {
        Q_OBJECT
    public:
        MainWindow(const std::string &shader_dir,
                   const SimulationConfig &cfg,
                   SimulationHost *host,
                   QWidget *parent = nullptr);
        ~MainWindow() override;

    private slots:
        void tick();

    protected:
        void dragEnterEvent(QDragEnterEvent *event) override;
        void dropEvent(QDropEvent *event) override;
        void closeEvent(QCloseEvent *event) override; // 未保存资产确认

    private:
        void build_ui(const std::string &shader_dir);
        void retranslate();
        void build_language_menu();
        void refresh_outliner();
        void apply_workspace();
        void open_asset(const QString &path);
        void show_outliner_context_menu(const QPoint &pos);
        void build_transform_toolbar();
        void set_transform_mode(TransformMode mode);
        void handle_drag_delta(float dx, float dy);
        void position_transform_toolbar();
        void build_plugin_menu();
        void load_plugin();
        void show_plugin_list();
        void execute_plugin(const std::string &name);
        void unload_plugin();
        void scan_plugin_directory();
        void reload_plugins();
        QString plugin_directory() const;
        void toggle_theme();
        void refresh_theme_action();
        void open_settings();

        SimulationConfig cfg_;
        std::unique_ptr<SimulationHost> host_;
        QVulkanInstance vulkan_instance_;
        QVulkanViewport *viewport_ = nullptr;
        QWidget *viewport_container_ = nullptr;

        // 变换模式 + 工具栏（gizmo 左侧）
        TransformMode transform_mode_ = TransformMode::Translate;
        QWidget *transform_toolbar_ = nullptr;
        QToolButton *btn_translate_ = nullptr;
        QToolButton *btn_rotate_ = nullptr;
        QToolButton *btn_scale_ = nullptr;

        // 停靠面板
        QDockWidget *outliner_dock_ = nullptr;
        QDockWidget *details_dock_ = nullptr;
        QDockWidget *content_dock_ = nullptr;
        QTreeWidget *outliner_tree_ = nullptr;
        QTabWidget *details_tabs_ = nullptr;
        ContentBrowser *content_browser_ = nullptr;

        // 面板（作为 Details 的 tab）
        TeleopPanel *teleop_;
        PhysicsPanel *physics_;
        RlPanel *rl_;
        QuantumPanel *quantum_;
        ConsciousnessPanel *consciousness_;
        BrainBridgePanel *brain_;
        CircuitPanel *circuit_;
        BlueprintPanel *blueprint_;
        SceneGraphPanel *scene_;
        LogPanel *log_;
        MetricsPanel *metrics_;
        FractalPanel *fractal_;
        EntityDetailsPanel *entity_details_;
        AssetDetailsPanel *asset_details_;

        // 状态栏
        QLabel *status_fps_;
        QLabel *status_step_;
        QLabel *status_backend_;
        QLabel *status_gpu_;

        // 菜单 / 工具栏（用于语言切换时重刷文案）
        QMenu *file_menu_ = nullptr;
        QMenu *sim_menu_ = nullptr;
        QMenu *view_menu_ = nullptr;
        QMenu *lang_menu_ = nullptr;
        QMenu *plugin_menu_ = nullptr;
        QMenu *tools_menu_ = nullptr;

        // 插件管理器（.rvdlp 拓展包）
        editor::PluginManager plugin_manager_;
        QAction *act_exit_ = nullptr;
        QAction *act_teleop_start_ = nullptr;
        QAction *act_teleop_stop_ = nullptr;
        QAction *act_pause_phys_ = nullptr;
        QAction *act_step_ = nullptr;
        QAction *act_view_outliner_ = nullptr;
        QAction *act_view_details_ = nullptr;
        QAction *act_view_content_ = nullptr;
        QToolBar *main_toolbar_ = nullptr;
        QAction *act_toolbar_teleop_ = nullptr;
        QAction *act_toolbar_pause_ = nullptr;
        QAction *act_toolbar_step_ = nullptr;
        QAction *act_toolbar_theme_ = nullptr;
        QAction *act_toggle_theme_ = nullptr;

        QTimer timer_;
        double last_time_ = 0.0;
        float fps_ = 0.0f;
        float frame_ms_ = 0.0f;
        bool meshes_uploaded_ = false;
        int selected_entity_index_ = -1;
        int cached_entity_count_ = 0;
        int frame_counter_ = 0; // 用于低频面板刷新节流
        double accumulator_ = 0.0; // 物理仿真固定步长累积器（解耦物理与渲染节奏）
    };
}