#include "main_window.h"
#include "simulation_host.hpp"
#include "vulkan_viewport.h"
#include "content_browser.h"
#include "project_store.h"
#include "script_editor.h"
#include "asset_viewers.h"
#include "theme_manager.h"
#include "settings_dialog.h"
#include "ui_utils.h"
#include "i18n/i18n.h"
#include "editor/rvdl_editor.hpp"

#include <QApplication>
#include <QSplitter>
#include <QTabWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QDateTime>
#include <QCoreApplication>
#include <QQuaternion>
#include <QTreeWidgetItemIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QMenu>
#include <QInputDialog>
#include <QActionGroup>
#include <QToolButton>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QSettings>
#include <QIcon>
#include <QProcess>
#include <QTemporaryFile>
#include <QDir>
#include <algorithm>

namespace rvdlrsp::gui
{

    MainWindow::MainWindow(const std::string &shader_dir, const SimulationConfig &cfg,
                           SimulationHost *host, QWidget *parent)
        : QMainWindow(parent), cfg_(cfg)
    {
        if (host)
            host_.reset(host);
        else
        {
            host_ = std::make_unique<SimulationHost>(cfg);
            host_->initialize_all();
        }

        // Vulkan 实例
        vulkan_instance_.setLayers(QByteArrayList() << "VK_LAYER_KHRONOS_validation");
        if (!vulkan_instance_.create())
            vulkan_instance_.setLayers(QByteArrayList());
        if (!vulkan_instance_.create())
            qWarning("Failed to create QVulkanInstance");

        build_ui(shader_dir);
        apply_workspace();
        // 同步 3D 视口初始主题（默认夜间；若用户已持久化白天则保持一致）
        if (viewport_)
            viewport_->set_dark_mode(ThemeManager::instance().isDark());

        // 恢复窗口状态（几何 + dock 布局，QSettings 持久化）
        QSettings settings;
        restoreGeometry(settings.value("mainwindow/geometry").toByteArray());
        restoreState(settings.value("mainwindow/state").toByteArray());

        last_time_ = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        connect(&timer_, &QTimer::timeout, this, &MainWindow::tick);
        timer_.start(16);
    }

    MainWindow::~MainWindow() = default;

    void MainWindow::build_ui(const std::string &shader_dir)
    {
        setWindowTitle(RVDLTR("Ravendrel RSP — 量子机器人仿真平台"));
        resize(fit_screen(0.85, QSize(1600, 900)));
        setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);

        // ── 菜单栏 ────────────────────────────────────────────────
        file_menu_ = menuBar()->addMenu(RVDLTR("文件(&F)"));
        act_exit_ = file_menu_->addAction(RVDLTR("退出(&Q)"), this, &QWidget::close, QKeySequence::Quit);
        sim_menu_ = menuBar()->addMenu(RVDLTR("仿真(&S)"));
        act_teleop_start_ = sim_menu_->addAction(RVDLTR("启动遥操作"), this, [this]()
                       { host_->start_teleop(); });
        act_teleop_stop_ = sim_menu_->addAction(RVDLTR("停止遥操作"), this, [this]()
                       { host_->stop_teleop(); });
        act_pause_phys_ = sim_menu_->addAction(RVDLTR("暂停物理"), this, [this]()
                       { host_->pause(); });
        act_step_ = sim_menu_->addAction(RVDLTR("单步"), this, [this]()
                       { host_->step_once(); });
        view_menu_ = menuBar()->addMenu(RVDLTR("视图(&V)"));
        act_view_outliner_ = view_menu_->addAction(RVDLTR("World Outliner"), this, [this]()
                        { if (outliner_dock_) outliner_dock_->show(); });
        act_view_details_ = view_menu_->addAction(RVDLTR("Details"), this, [this]()
                        { if (details_dock_) details_dock_->show(); });
        act_view_content_ = view_menu_->addAction(RVDLTR("Content Browser"), this, [this]()
                        { if (content_dock_) content_dock_->show(); });
        view_menu_->addSeparator();
        act_toggle_theme_ = view_menu_->addAction(QString(), this, [this]() { toggle_theme(); });
        act_toggle_theme_->setCheckable(true);
        refresh_theme_action();
        tools_menu_ = menuBar()->addMenu(RVDLTR("工具(&T)"));
        tools_menu_->addAction(RVDLTR("设置(&S)"), this, &MainWindow::open_settings);
        build_language_menu();
        build_plugin_menu();

        // ── 工具栏 ────────────────────────────────────────────────
        main_toolbar_ = addToolBar(RVDLTR("主工具栏"));
        main_toolbar_->setMovable(false);
        act_toolbar_teleop_ = main_toolbar_->addAction(QIcon(":/icons/play.svg"), RVDLTR("遥操作"), this, [this]()
                           { host_->start_teleop(); });
        act_toolbar_pause_ = main_toolbar_->addAction(QIcon(":/icons/pause.svg"), RVDLTR("暂停"), this, [this]()
                           { host_->pause(); });
        act_toolbar_step_ = main_toolbar_->addAction(QIcon(":/icons/step.svg"), RVDLTR("单步"), this, [this]()
                           { host_->step_once(); });
        main_toolbar_->addSeparator();
        act_toolbar_theme_ = main_toolbar_->addAction(QString(), this, [this]() { toggle_theme(); });
        act_toolbar_theme_->setToolTip(RVDLTR("切换白天/夜间模式"));
        refresh_theme_action();

        // ── 中央 3D 视口 ──────────────────────────────────────────
        viewport_ = new QVulkanViewport(shader_dir);
        viewport_->setVulkanInstance(&vulkan_instance_);
        viewport_container_ = QWidget::createWindowContainer(viewport_, this);
        setCentralWidget(viewport_container_);
        setAcceptDrops(true);

        // ── 变换工具栏（gizmo 左侧，平移/旋转/缩放）───────────────
        build_transform_toolbar();

        // ── 左侧 World Outliner（场景层级树）──────────────────────
        outliner_dock_ = new QDockWidget(RVDLTR("World Outliner"), this);
        outliner_tree_ = new QTreeWidget(outliner_dock_);
        outliner_tree_->setHeaderLabel(RVDLTR("场景层级"));
        outliner_tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        outliner_dock_->setWidget(outliner_tree_);
        addDockWidget(Qt::LeftDockWidgetArea, outliner_dock_);

        // ── 右侧 Details（属性面板，含 11 个 tab）─────────────────
        details_dock_ = new QDockWidget(RVDLTR("Details"), this);
        details_tabs_ = new QTabWidget(details_dock_);
        teleop_ = new TeleopPanel(details_tabs_);
        physics_ = new PhysicsPanel(details_tabs_);
        rl_ = new RlPanel(details_tabs_);
        quantum_ = new QuantumPanel(details_tabs_);
        consciousness_ = new ConsciousnessPanel(details_tabs_);
        brain_ = new BrainBridgePanel(details_tabs_);
        circuit_ = new CircuitPanel(details_tabs_);
        blueprint_ = new BlueprintPanel(details_tabs_);
        scene_ = new SceneGraphPanel(details_tabs_);
        log_ = new LogPanel(details_tabs_);
        metrics_ = new MetricsPanel(details_tabs_);
        fractal_ = new FractalPanel(details_tabs_);
        fractal_->set_params(cfg_.fractal);
        entity_details_ = new EntityDetailsPanel(details_tabs_);
        asset_details_ = new AssetDetailsPanel(details_tabs_);

        details_tabs_->addTab(asset_details_, RVDLTR("资产属性"));
        details_tabs_->addTab(entity_details_, RVDLTR("实体属性"));
        details_tabs_->addTab(teleop_, RVDLTR("遥操作"));
        details_tabs_->addTab(physics_, RVDLTR("物理"));
        details_tabs_->addTab(rl_, RVDLTR("强化学习"));
        details_tabs_->addTab(quantum_, RVDLTR("量子"));
        details_tabs_->addTab(consciousness_, RVDLTR("意识"));
        details_tabs_->addTab(brain_, RVDLTR("脑机桥"));
        details_tabs_->addTab(circuit_, RVDLTR("电路"));
        details_tabs_->addTab(blueprint_, RVDLTR("蓝图"));
        details_tabs_->addTab(scene_, RVDLTR("场景图"));
        details_tabs_->addTab(log_, RVDLTR("日志"));
        details_tabs_->addTab(metrics_, RVDLTR("指标"));
        details_tabs_->addTab(fractal_, RVDLTR("分形地形"));
        details_dock_->setWidget(details_tabs_);
        details_dock_->setMinimumWidth(380);
        addDockWidget(Qt::RightDockWidgetArea, details_dock_);

        // ── 底部 Content Browser + 日志（tab 切换）─────────────────
        content_dock_ = new QDockWidget(RVDLTR("Content Browser"), this);
        auto *bottom_tabs = new QTabWidget(content_dock_);
        content_browser_ = new ContentBrowser(
            QString::fromStdString(content_directory_for(cfg_.project_name)), bottom_tabs);
        log_ = new LogPanel(bottom_tabs);
        bottom_tabs->addTab(content_browser_, RVDLTR("内容浏览器"));
        bottom_tabs->addTab(log_, RVDLTR("日志"));
        content_dock_->setWidget(bottom_tabs);
        addDockWidget(Qt::BottomDockWidgetArea, content_dock_);

        // ── 状态栏 ────────────────────────────────────────────────
        status_fps_ = new QLabel(RVDLTR("FPS: %1").arg(0), this);
        status_step_ = new QLabel(RVDLTR("步数: %1").arg(0), this);
        status_backend_ = new QLabel(RVDLTR("后端: %1").arg(QString::fromStdString(host_->quantum_backend())), this);
        status_gpu_ = new QLabel(RVDLTR("GPU: 未启用"), this);
        statusBar()->addWidget(status_fps_);
        statusBar()->addPermanentWidget(status_step_);
        statusBar()->addPermanentWidget(status_backend_);
        statusBar()->addPermanentWidget(status_gpu_);

        // ── 面板信号 → host ───────────────────────────────────────
        connect(teleop_, &TeleopPanel::startTeleop, this, [this]()
                { host_->start_teleop(); });
        connect(teleop_, &TeleopPanel::stopTeleop, this, [this]()
                { host_->stop_teleop(); });
        connect(physics_, &PhysicsPanel::pauseSim, this, [this]()
                { host_->pause(); });
        connect(physics_, &PhysicsPanel::stepOnce, this, [this]()
                { host_->step_once(); });
        connect(rl_, &RlPanel::startTraining, this, [this]()
                { host_->start_training(); });
        connect(rl_, &RlPanel::stopTraining, this, [this]()
                { host_->stop_training(); });
        connect(fractal_, &FractalPanel::applyRequested, this, [this]()
                {
            if (host_->set_fractal_params(fractal_->params()))
                meshes_uploaded_ = false; });

        // ── Outliner 右键菜单 ──────────────────────────────────────
        connect(outliner_tree_, &QTreeWidget::customContextMenuRequested, this,
                &MainWindow::show_outliner_context_menu);

        // ── Outliner 选中 → Details 联动 ──────────────────────────
        connect(outliner_tree_, &QTreeWidget::itemSelectionChanged, this, [this]()
                {
            auto *item = outliner_tree_->currentItem();
            if (!item)
            {
                selected_entity_index_ = -1;
                entity_details_->clear_entity();
                return;
            }
            int idx = item->data(0, Qt::UserRole).toInt();
            selected_entity_index_ = idx;
            const auto &ents = host_->scene_entities();
            if (idx < 0 || idx >= static_cast<int>(ents.size()))
            {
                entity_details_->clear_entity();
                return;
            }
            const auto &e = ents[static_cast<size_t>(idx)];
            entity_details_->show_entity(idx, e.name, e.kind,
                                         e.position.x, e.position.y, e.position.z,
                                         e.mass, e.collider,
                                         e.rotation.x, e.rotation.y, e.rotation.z,
                                         e.scale.x, e.scale.y, e.scale.z); });

        // ── 属性编辑 → 写回物理刚体 ───────────────────────────────
        connect(entity_details_, &EntityDetailsPanel::positionEdited, this,
                [this](int idx, double x, double y, double z)
                {
                    host_->set_entity_position(idx, {x, y, z});
                });

        // ── 旋转编辑 → 写回导入模型（XYZ 顺序）───────────────────
        connect(entity_details_, &EntityDetailsPanel::rotationEdited, this,
                [this](int idx, double x, double y, double z)
                {
                    // 欧拉角(度) → 四元数，应用顺序 X → Y → Z
                    auto deg = [](double d)
                    { return d * 3.14159265358979323846 / 180.0; };
                    qpc::Quat q = qpc::Quat::axis_angle({1, 0, 0}, deg(x)) *
                                  qpc::Quat::axis_angle({0, 1, 0}, deg(y)) *
                                  qpc::Quat::axis_angle({0, 0, 1}, deg(z));
                    host_->set_entity_rotation(idx, q);
                });

        // ── 缩放编辑 → 写回导入模型 ───────────────────────────────
        connect(entity_details_, &EntityDetailsPanel::scaleEdited, this,
                [this](int idx, double x, double y, double z)
                {
                    host_->set_entity_scale(idx, {x, y, z});
                });

        // ── 视口左键拖拽 → 按当前变换模式操作选中实体 ─────────────
        connect(viewport_, &QVulkanViewport::dragDelta, this,
                [this](float dx, float dy) { handle_drag_delta(dx, dy); });

        // ── 删除导入实体 ───────────────────────────────────────────
        connect(entity_details_, &EntityDetailsPanel::deleteRequested, this,
                [this](int idx)
                {
                    if (host_->remove_imported_mesh(idx))
                    {
                        selected_entity_index_ = -1;
                        cached_entity_count_ = 0; // 触发 Outliner 重建
                        entity_details_->clear_entity();
                        meshes_uploaded_ = false;
                        log_->refresh(*host_);
                    }
                });

        // ── 内容浏览器选中资产 → 资产属性面板 ─────────────────────
        connect(content_browser_, &ContentBrowser::assetSelected, this,
                [this](const QString &path)
                {
                    AssetType t = asset_type_from_path(path);
                    int status = content_browser_->store()->status_of(path);
                    asset_details_->show_asset(path, asset_type_name(t), status);
                    details_tabs_->setCurrentWidget(asset_details_);
                    details_dock_->show();
                });

        // ── 内容浏览器右键"打开" → 按类型分发 ─────────────────────
        connect(content_browser_, &ContentBrowser::assetActivated, this,
                &MainWindow::open_asset);

        // ── 资产编辑 dirty 状态 → 内容浏览器角标 ──────────────────
        connect(asset_details_, &AssetDetailsPanel::dirtyChanged, this,
                [this](const QString &path, bool dirty)
                {
                    content_browser_->set_dirty(path, dirty);
                });
    }

    void MainWindow::build_language_menu()
    {
        lang_menu_ = menuBar()->addMenu(RVDLTR("语言(&L)"));
        auto *group = new QActionGroup(this);
        group->setExclusive(true);

        const QStringList langs = I18n::languages();
        const QString cur = I18n::instance().current();
        for (const QString &code : langs)
        {
            QAction *a = lang_menu_->addAction(I18n::languageName(code));
            a->setCheckable(true);
            a->setData(code);
            a->setChecked(code == cur);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, code]()
                    {
                        I18n::instance().setLanguage(code);
                        retranslate();
                    });
        }
    }

    void MainWindow::retranslate()
    {
        setWindowTitle(RVDLTR("Ravendrel RSP — 量子机器人仿真平台"));

        if (file_menu_) file_menu_->setTitle(RVDLTR("文件(&F)"));
        if (act_exit_) act_exit_->setText(RVDLTR("退出(&Q)"));
        if (sim_menu_) sim_menu_->setTitle(RVDLTR("仿真(&S)"));
        if (act_teleop_start_) act_teleop_start_->setText(RVDLTR("启动遥操作"));
        if (act_teleop_stop_) act_teleop_stop_->setText(RVDLTR("停止遥操作"));
        if (act_pause_phys_) act_pause_phys_->setText(RVDLTR("暂停物理"));
        if (act_step_) act_step_->setText(RVDLTR("单步"));
        if (view_menu_) view_menu_->setTitle(RVDLTR("视图(&V)"));
        if (act_view_outliner_) act_view_outliner_->setText(RVDLTR("World Outliner"));
        if (act_view_details_) act_view_details_->setText(RVDLTR("Details"));
        if (act_view_content_) act_view_content_->setText(RVDLTR("Content Browser"));
        if (lang_menu_) lang_menu_->setTitle(RVDLTR("语言(&L)"));
        if (plugin_menu_) plugin_menu_->setTitle(RVDLTR("插件(&P)"));
        if (tools_menu_) tools_menu_->setTitle(RVDLTR("工具(&T)"));

        if (main_toolbar_) main_toolbar_->setWindowTitle(RVDLTR("主工具栏"));
        if (act_toolbar_teleop_) act_toolbar_teleop_->setText(RVDLTR("遥操作"));
        if (act_toolbar_pause_) act_toolbar_pause_->setText(RVDLTR("暂停"));
        if (act_toolbar_step_) act_toolbar_step_->setText(RVDLTR("单步"));

        if (outliner_dock_) outliner_dock_->setWindowTitle(RVDLTR("World Outliner"));
        if (details_dock_) details_dock_->setWindowTitle(RVDLTR("Details"));
        if (content_dock_) content_dock_->setWindowTitle(RVDLTR("Content Browser"));
        if (outliner_tree_) outliner_tree_->setHeaderLabel(RVDLTR("场景层级"));

        if (details_tabs_)
        {
            details_tabs_->setTabText(0, RVDLTR("资产属性"));
            details_tabs_->setTabText(1, RVDLTR("实体属性"));
            details_tabs_->setTabText(2, RVDLTR("遥操作"));
            details_tabs_->setTabText(3, RVDLTR("物理"));
            details_tabs_->setTabText(4, RVDLTR("强化学习"));
            details_tabs_->setTabText(5, RVDLTR("量子"));
            details_tabs_->setTabText(6, RVDLTR("意识"));
            details_tabs_->setTabText(7, RVDLTR("脑机桥"));
            details_tabs_->setTabText(8, RVDLTR("电路"));
            details_tabs_->setTabText(9, RVDLTR("蓝图"));
            details_tabs_->setTabText(10, RVDLTR("场景图"));
            details_tabs_->setTabText(11, RVDLTR("日志"));
            details_tabs_->setTabText(12, RVDLTR("指标"));
            details_tabs_->setTabText(13, RVDLTR("分形地形"));
        }

        refresh_theme_action();
    }

    void MainWindow::toggle_theme()
    {
        ThemeManager::instance().toggle();
        const bool dark = ThemeManager::instance().isDark();
        ThemeManager::instance().apply(*qApp);
        if (viewport_)
            viewport_->set_dark_mode(dark); // 3D 背景色/光照联动
        refresh_theme_action();
    }

    void MainWindow::refresh_theme_action()
    {
        const bool dark = ThemeManager::instance().isDark();
        if (act_toggle_theme_)
        {
            act_toggle_theme_->setChecked(dark);
            act_toggle_theme_->setText(dark ? RVDLTR("夜间模式") : RVDLTR("白天模式"));
        }
        if (act_toolbar_theme_)
            act_toolbar_theme_->setIcon(QIcon(dark ? QStringLiteral(":/icons/theme-dark.svg")
                                                   : QStringLiteral(":/icons/theme-light.svg")));
    }

    void MainWindow::open_settings()
    {
        SettingsDialog dlg(this);
        connect(&dlg, &SettingsDialog::settingsChanged, this, [this]()
                {
                    retranslate();
                    refresh_theme_action(); });
        dlg.exec();
    }

    void MainWindow::open_asset(const QString &path)
    {
        const AssetType t = asset_type_from_path(path);

        switch (t)
        {
        case AssetType::Script:
        {
            // rvdl 脚本 → 新窗口脚本编辑器
            auto *win = new ScriptEditorWindow(path);
            win->show();
            // dirty 联动到内容浏览器角标
            connect(win, &ScriptEditorWindow::dirtyChanged, this,
                    [this](const QString &p, bool dirty)
                    {
                        content_browser_->set_dirty(p, dirty);
                    });
            break;
        }
        case AssetType::Audio:
        {
            auto *win = new AudioPlayerWindow(path);
            win->show();
            break;
        }
        case AssetType::Texture:
        {
            auto *win = new ImageViewerWindow(path);
            win->show();
            break;
        }
        case AssetType::Mesh:
        {
            auto *win = new ModelViewerWindow(path);
            win->show();
            break;
        }
        case AssetType::Scene:
        {
            // 加载场景
            if (host_->load_scene_file(path.toStdString()))
            {
                log_->refresh(*host_);
                meshes_uploaded_ = false; // 触发网格重新上传
                viewport_->reset_camera(); // 视口自动聚焦（重置相机）
            }
            break;
        }
        default:
        {
            // 其它（机器人/材质/蓝图/未知）→ 资产属性面板
            int status = content_browser_->store()->status_of(path);
            asset_details_->show_asset(path, asset_type_name(t), status);
            details_tabs_->setCurrentWidget(asset_details_);
            details_dock_->show();
            break;
        }
        }
    }

    void MainWindow::dragEnterEvent(QDragEnterEvent *event)
    {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            QMainWindow::dragEnterEvent(event);
    }

    void MainWindow::dropEvent(QDropEvent *event)
    {
        const QMimeData *mime = event->mimeData();
        if (!mime || !mime->hasUrls())
        {
            QMainWindow::dropEvent(event);
            return;
        }

        for (const QUrl &url : mime->urls())
        {
            if (!url.isLocalFile())
                continue;
            QString path = url.toLocalFile();
            AssetType t = asset_type_from_path(path);
            if (t == AssetType::Mesh)
            {
                // 模型/骨骼文件 → 导入到 3D 场景
                if (host_->import_mesh(path.toStdString()))
                {
                    meshes_uploaded_ = false; // 触发下一帧重新上传网格
                    log_->refresh(*host_);
                }
            }
        }
        event->acceptProposedAction();
    }

    void MainWindow::tick()
    {
        double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        double dt = now - last_time_;
        last_time_ = now;
        if (dt > 0.25)
            dt = 0.25;

        frame_ms_ = static_cast<float>(dt * 1000.0);
        if (frame_ms_ > 0.0f)
            fps_ = fps_ * 0.9f + (1000.0f / frame_ms_) * 0.1f;

        // 推进真实仿真内核：固定时间步长累积，解耦物理与渲染节奏。
        // 渲染每帧刷新，物理按固定步长（1/60s）推进，避免帧率波动影响物理精度。
        accumulator_ += dt;
        constexpr double kFixedStep = 1.0 / 60.0;
        int step_count = 0;
        while (accumulator_ >= kFixedStep && step_count < 8)
        {
            host_->step(static_cast<float>(kFixedStep));
            accumulator_ -= kFixedStep;
            ++step_count;
        }

        // 上传场景网格（首次）
        if (!meshes_uploaded_ && host_->scene_part_count() > 0)
        {
            viewport_->set_meshes(host_->scene_meshes());
            meshes_uploaded_ = true;
        }

        // 更新 3D 视口：仅场景实例与太阳（相机由视口交互维护）
        std::vector<InstanceTransform> instances;
        for (const auto &inst : host_->scene_instances())
        {
            InstanceTransform it;
            it.mesh_id = static_cast<int>(inst.mesh_id);
            it.position = QVector3D(static_cast<float>(inst.position.x),
                                    static_cast<float>(inst.position.y),
                                    static_cast<float>(inst.position.z));
            it.rotation = QQuaternion(static_cast<float>(inst.orientation.w),
                                      static_cast<float>(inst.orientation.x),
                                      static_cast<float>(inst.orientation.y),
                                      static_cast<float>(inst.orientation.z));
            it.scale = QVector3D(static_cast<float>(inst.scale.x),
                                 static_cast<float>(inst.scale.y),
                                 static_cast<float>(inst.scale.z));
            instances.push_back(it);
        }
        float sun_dir[3] = { cfg_.env.sun_dir[0], cfg_.env.sun_dir[1], cfg_.env.sun_dir[2] };
        viewport_->set_scene(instances, sun_dir);

        // 更新 World Outliner（场景层级树 + 选中联动）
        refresh_outliner();

        // 回读 UI 参数 → host
        host_->set_joint_angle(teleop_->joint_angle());
        host_->set_gravity(physics_->gravity());
        host_->set_solver_iterations(physics_->solver_iterations());

        // 刷新面板：实时面板每帧刷新，低频面板节流（降低每帧重绘/重建开销）
        teleop_->refresh(*host_);
        physics_->refresh(*host_);
        metrics_->refresh(*host_, fps_, frame_ms_);

        // 低频面板（RL/量子/意识/脑机桥/电路/蓝图/场景图/日志）每 20 帧（约 0.33s）刷新
        ++frame_counter_;
        if (frame_counter_ % 20 == 0)
        {
            rl_->refresh(*host_);
            quantum_->refresh(*host_);
            consciousness_->refresh(*host_);
            brain_->refresh(*host_);
            circuit_->refresh(*host_);
            blueprint_->refresh(*host_);
            scene_->refresh(*host_);
            log_->refresh(*host_);
        }

        // 状态栏
        status_fps_->setText(RVDLTR("FPS: %1").arg(fps_, 0, 'f', 1));
        status_step_->setText(RVDLTR("步数: %1").arg(host_->sim_step()));
        status_backend_->setText(RVDLTR("后端: %1").arg(QString::fromStdString(host_->quantum_backend())));
        // GPU 状态：如实反馈后端是否真的启用了 GPU 加速
        status_gpu_->setText(host_->gpu_accel_active()
                                 ? RVDLTR("GPU: 已启用")
                                 : (cfg_.gpu_accel ? RVDLTR("GPU: 已勾选但后端未启用")
                                                   : RVDLTR("GPU: 未启用")));

        // 变换工具栏跟随视口尺寸（gizmo 左侧）
        position_transform_toolbar();
    }

    // ─── World Outliner 刷新 + 选中联动 ─────────────────────────────
    void MainWindow::refresh_outliner()
    {
        const auto &ents = host_->scene_entities();
        if (ents.empty())
            return;

        // 实体数量变化时才重建树（避免每帧重建打断用户交互）
        if (cached_entity_count_ != static_cast<int>(ents.size()))
        {
            cached_entity_count_ = static_cast<int>(ents.size());

            outliner_tree_->blockSignals(true);
            outliner_tree_->clear();

            QTreeWidgetItem *robot_root = nullptr;
            for (size_t i = 0; i < ents.size(); ++i)
            {
                const auto &e = ents[i];
                QTreeWidgetItem *item;
                if (e.kind == "part")
                {
                    if (!robot_root)
                    {
                        robot_root = new QTreeWidgetItem(outliner_tree_);
                        robot_root->setText(0, "humanoid_robot");
                        robot_root->setData(0, Qt::UserRole, -1);
                    }
                    item = new QTreeWidgetItem(robot_root);
                }
                else
                {
                    item = new QTreeWidgetItem(outliner_tree_);
                }
                item->setText(0, QString("%1  [%2]").arg(QString::fromStdString(e.name), QString::fromStdString(e.kind)));
                item->setData(0, Qt::UserRole, static_cast<int>(i));
            }
            outliner_tree_->expandAll();
            outliner_tree_->blockSignals(false);

            // 恢复选中高亮
            if (selected_entity_index_ >= 0)
            {
                QTreeWidgetItemIterator it(outliner_tree_);
                while (*it)
                {
                    if ((*it)->data(0, Qt::UserRole).toInt() == selected_entity_index_)
                    {
                        outliner_tree_->setCurrentItem(*it);
                        break;
                    }
                    ++it;
                }
            }
        }

        // 每帧刷新选中实体的 Details 数值（跟随物理状态）
        if (selected_entity_index_ >= 0 && selected_entity_index_ < static_cast<int>(ents.size()))
        {
            const auto &e = ents[static_cast<size_t>(selected_entity_index_)];
            entity_details_->show_entity(selected_entity_index_, e.name, e.kind,
                                         e.position.x, e.position.y, e.position.z,
                                         e.mass, e.collider,
                                         e.rotation.x, e.rotation.y, e.rotation.z,
                                         e.scale.x, e.scale.y, e.scale.z);
        }
    }

    // ─── 工作区切换：按配置聚焦不同面板/停靠 ───────────────────────
    void MainWindow::apply_workspace()
    {
        switch (cfg_.workspace)
        {
        case Workspace::RobotControl:
            if (details_tabs_)
                details_tabs_->setCurrentIndex(2); // 遥操作
            if (outliner_dock_)
                outliner_dock_->show();
            break;
        case Workspace::SceneEdit:
            if (details_tabs_)
                details_tabs_->setCurrentIndex(10); // 场景图
            if (outliner_dock_)
                outliner_dock_->show();
            break;
        case Workspace::SimDebug:
        default:
            if (details_tabs_)
                details_tabs_->setCurrentIndex(2);
            break;
        }
    }

    void MainWindow::show_outliner_context_menu(const QPoint &pos)
    {
        auto *item = outliner_tree_->itemAt(pos);
        if (!item)
            return;
        int idx = item->data(0, Qt::UserRole).toInt();
        const auto &ents = host_->scene_entities();
        if (idx < 0 || idx >= static_cast<int>(ents.size()))
            return;
        const auto &e = ents[static_cast<size_t>(idx)];

        QMenu menu(this);

        // 重命名（仅导入模型）
        QAction *ren = menu.addAction(RVDLTR("重命名"));
        ren->setEnabled(e.kind == "imported");
        connect(ren, &QAction::triggered, this, [this, idx, name = QString::fromStdString(e.name)]()
                {
            bool ok = false;
            QString n = QInputDialog::getText(this, RVDLTR("重命名实体"), RVDLTR("名称："),
                                              QLineEdit::Normal, name, &ok);
            if (ok && !n.trimmed().isEmpty() && host_->rename_imported_mesh(idx, n.toStdString()))
            {
                cached_entity_count_ = 0;   // 触发 Outliner 重建
                refresh_outliner();
            } });

        // 删除（仅导入模型）
        QAction *del = menu.addAction(RVDLTR("删除实体"));
        del->setEnabled(e.kind == "imported");
        connect(del, &QAction::triggered, this, [this, idx]()
                {
            if (host_->remove_imported_mesh(idx))
            {
                selected_entity_index_ = -1;
                cached_entity_count_ = 0;   // 触发 Outliner 重建
                entity_details_->clear_entity();
                meshes_uploaded_ = false;
                log_->refresh(*host_);
            } });

        menu.exec(outliner_tree_->viewport()->mapToGlobal(pos));
    }

    // ─── 变换工具栏（gizmo 左侧，平移/旋转/缩放）─────────
    void MainWindow::build_transform_toolbar()
    {
        transform_toolbar_ = new QWidget(viewport_container_);
        transform_toolbar_->setObjectName("transformToolbar");
        transform_toolbar_->setStyleSheet(
            "QWidget#transformToolbar { background: rgba(30,30,34,200); border-radius: 4px; }");

        auto *lay = new QHBoxLayout(transform_toolbar_);
        lay->setContentsMargins(2, 2, 2, 2);
        lay->setSpacing(2);

        auto make_btn = [&](const QString &tip, const QString &text) {
            auto *b = new QToolButton(transform_toolbar_);
            b->setText(text);
            b->setToolTip(tip);
            b->setCheckable(true);
            b->setFixedSize(28, 28);
            return b;
        };

        btn_translate_ = make_btn(RVDLTR("移动 (W)"), "T");
        btn_rotate_    = make_btn(RVDLTR("旋转 (E)"), "R");
        btn_scale_     = make_btn(RVDLTR("缩放 (R)"), "S");

        auto *grp = new QButtonGroup(this);
        grp->setExclusive(true);
        grp->addButton(btn_translate_);
        grp->addButton(btn_rotate_);
        grp->addButton(btn_scale_);

        lay->addWidget(btn_translate_);
        lay->addWidget(btn_rotate_);
        lay->addWidget(btn_scale_);

        connect(btn_translate_, &QToolButton::clicked, this, [this]() { set_transform_mode(TransformMode::Translate); });
        connect(btn_rotate_,    &QToolButton::clicked, this, [this]() { set_transform_mode(TransformMode::Rotate); });
        connect(btn_scale_,     &QToolButton::clicked, this, [this]() { set_transform_mode(TransformMode::Scale); });

        // 初始状态：Translate
        set_transform_mode(TransformMode::Translate);
        position_transform_toolbar();
    }

    void MainWindow::set_transform_mode(TransformMode mode)
    {
        transform_mode_ = mode;
        btn_translate_->setChecked(mode == TransformMode::Translate);
        btn_rotate_->setChecked(mode == TransformMode::Rotate);
        btn_scale_->setChecked(mode == TransformMode::Scale);
    }

    // 工具栏定位：右上角 gizmo 的左侧（gizmo 固定渲染在 (w-132, 12) 起的 120x120 区域）
    void MainWindow::position_transform_toolbar()
    {
        if (!transform_toolbar_ || !viewport_container_)
            return;
        int w = viewport_container_->width();
        const int gizmo_size = 120;
        const int margin = 12;
        const int gap = 8;
        int tw = transform_toolbar_->sizeHint().width();
        int th = transform_toolbar_->sizeHint().height();
        int x = w - margin - gizmo_size - gap - tw;
        int y = margin + (gizmo_size - th) / 2;
        transform_toolbar_->move(x, y);
        transform_toolbar_->raise();
    }

    // 左键拖拽：按当前变换模式操作选中实体
    void MainWindow::handle_drag_delta(float dx, float dy)
    {
        if (selected_entity_index_ < 0)
            return;
        const auto &ents = host_->scene_entities();
        if (selected_entity_index_ >= static_cast<int>(ents.size()))
            return;
        const auto &e = ents[static_cast<size_t>(selected_entity_index_)];
        if (e.kind != "imported")
            return;

        switch (transform_mode_)
        {
        case TransformMode::Translate:
        {
            // 屏幕拖拽 → 相机空间 right/up → 世界空间移动
            QVector3D fwd, right, up;
            viewport_->get_camera_vectors(fwd, right, up);
            // 平移在水平面（忽略 up 的垂直分量，用世界 XZ 平面）
            QVector3D world_right(right.x(), 0.0f, right.z());
            if (world_right.lengthSquared() < 1e-6f)
                world_right = QVector3D(1.0f, 0.0f, 0.0f);
            world_right.normalize();
            QVector3D world_fwd = QVector3D::crossProduct(QVector3D(0, 1, 0), world_right).normalized();

            const double scale = 0.02;
            double wx = (world_right.x() * dx + world_fwd.x() * (-dy)) * scale;
            double wz = (world_right.z() * dx + world_fwd.z() * (-dy)) * scale;
            qpc::Vec3 pos = e.position;
            pos.x += wx;
            pos.z += wz;
            host_->set_entity_position(selected_entity_index_, pos);
            break;
        }
        case TransformMode::Rotate:
        {
            // 绕 Y 轴（水平）+ 绕 X 轴（垂直）
            const double deg_per_px = 0.3;
            qpc::Quat ry = qpc::Quat::axis_angle({0, 1, 0}, dx * deg_per_px * 3.14159265358979323846 / 180.0);
            qpc::Quat rx = qpc::Quat::axis_angle({1, 0, 0}, dy * deg_per_px * 3.14159265358979323846 / 180.0);
            qpc::Quat q = ry * e.rotation * rx;
            host_->set_entity_rotation(selected_entity_index_, q);
            break;
        }
        case TransformMode::Scale:
        {
            // 垂直拖拽 → 统一缩放（向上放大，向下缩小）
            const double scale_per_px = 0.005;
            double factor = 1.0 - dy * scale_per_px;
            factor = std::clamp(factor, 0.05, 20.0);
            qpc::Vec3 s = e.scale;
            s.x *= factor;
            s.y *= factor;
            s.z *= factor;
            host_->set_entity_scale(selected_entity_index_, s);
            break;
        }
        }
    }

    // ─── 插件菜单（.rvdlp 拓展包）────────────────────────────────
    void MainWindow::build_plugin_menu()
    {
        plugin_menu_ = menuBar()->addMenu(RVDLTR("插件(&P)"));
        plugin_menu_->addAction(RVDLTR("加载插件…"), this, [this]() { load_plugin(); });
        plugin_menu_->addAction(RVDLTR("插件列表…"), this, [this]() { show_plugin_list(); });
        plugin_menu_->addAction(RVDLTR("卸载插件…"), this, [this]() { unload_plugin(); });
        plugin_menu_->addSeparator();
        plugin_menu_->addAction(RVDLTR("扫描插件目录"), this, [this]() { scan_plugin_directory(); });
        plugin_menu_->addAction(RVDLTR("热重载插件"), this, [this]() { reload_plugins(); });

        // 启动时自动扫描插件目录
        scan_plugin_directory();
    }

    void MainWindow::reload_plugins()
    {
        std::vector<std::string> errors;
        int n = plugin_manager_.reload_changed(errors);
        if (n == 0 && errors.empty())
        {
            QMessageBox::information(this, RVDLTR("热重载插件"), RVDLTR("没有检测到插件文件变化。"));
            return;
        }
        QString msg = RVDLTR("已热重载 %1 个插件。").arg(n);
        if (!errors.empty())
        {
            QStringList el;
            for (const auto &e : errors)
                el << QString::fromStdString(e);
            msg += "\n" + RVDLTR("失败：\n%1").arg(el.join("\n"));
        }
        QMessageBox::information(this, RVDLTR("热重载插件"), msg);
    }

    // 插件目录：可执行文件同级的 plugins/ 目录（不存在则跳过）
    QString MainWindow::plugin_directory() const
    {
        return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    }

    void MainWindow::scan_plugin_directory()
    {
        std::vector<std::string> errors;
        int n = plugin_manager_.scan_directory(plugin_directory().toStdString(), errors);
        if (n == 0 && errors.empty())
        {
            // 目录不存在或为空：**不再静默**。用户点了「扫描插件」却毫无反馈时，
            // 无法区分「没扫到」和「扫了但什么都没发生」—— 而前者才是常态
            // （plugins/ 目录默认不存在）。在状态栏给一行说明。
            if (statusBar())
                statusBar()->showMessage(
                    RVDLTR("未找到插件（目录 %1 不存在或为空）").arg(plugin_directory()),
                    5000);
            return;
        }
        if (!errors.empty())
        {
            QStringList el;
            for (const auto &e : errors)
                el << QString::fromStdString(e);
            QMessageBox::warning(this, RVDLTR("扫描插件目录"),
                                 RVDLTR("部分插件加载失败：\n%1").arg(el.join("\n")));
        }
    }

    void MainWindow::unload_plugin()
    {
        const auto &plugs = plugin_manager_.plugins();
        if (plugs.empty())
        {
            QMessageBox::information(this, RVDLTR("卸载插件"), RVDLTR("尚未加载任何插件。"));
            return;
        }
        QStringList names;
        for (const auto &p : plugs)
            names << QString::fromStdString(p.name);

        bool ok = false;
        QString sel = QInputDialog::getItem(this, RVDLTR("卸载插件"),
                                            RVDLTR("选择要卸载的插件："), names, 0, false, &ok);
        if (!ok || sel.isEmpty())
            return;
        if (plugin_manager_.unload(sel.toStdString()))
            QMessageBox::information(this, RVDLTR("卸载插件"),
                                     RVDLTR("插件 %1 已卸载。").arg(sel));
    }

    void MainWindow::load_plugin()
    {
        QString path = QFileDialog::getOpenFileName(this, RVDLTR("加载插件"),
                                                    QString(), RVDLTR("Ravendrel 插件包 (*.rvdlp)"));
        if (path.isEmpty())
            return;

        std::string err;
        std::string loaded_name;
        if (!plugin_manager_.load_file(path.toStdString(), err, &loaded_name))
        {
            QMessageBox::warning(this, RVDLTR("加载插件"), QString::fromStdString(err));
            return;
        }

        // 按实际加载的插件名定位（同名替换时 plugins().back() 不可靠）。
        const auto *p = plugin_manager_.find(loaded_name);
        if (!p)
        {
            QMessageBox::information(this, RVDLTR("加载插件"), RVDLTR("插件已加载。"));
            return;
        }
        if (!p->signature_ok)
        {
            // 正常情况下不可达（load_file 默认拒绝未签名插件）；
            // 仅在显式放行模式（RVDL_PLUGIN_ALLOW_UNSIGNED=1 或
            // set_require_signature(false)）下才会走到这里。
            QMessageBox::warning(this, RVDLTR("加载插件"),
                                 RVDLTR("插件 %1 未签名或签名校验失败（当前为显式允许未签名的开发模式）。")
                                     .arg(QString::fromStdString(p->name)));
        }
        else
        {
            QMessageBox::information(this, RVDLTR("加载插件"),
                                     RVDLTR("插件已加载：%1 v%2（签名有效）").arg(
                                         QString::fromStdString(p->name),
                                         QString::fromStdString(p->version)));
        }
    }

    void MainWindow::show_plugin_list()
    {
        const auto &plugs = plugin_manager_.plugins();
        if (plugs.empty())
        {
            QMessageBox::information(this, RVDLTR("插件列表"), RVDLTR("尚未加载任何插件。"));
            return;
        }

        QStringList names;
        for (const auto &p : plugs)
        {
            names << QString("%1  v%2  —  %3")
                         .arg(QString::fromStdString(p.name),
                              QString::fromStdString(p.version),
                              QString::fromStdString(p.description.empty() ? "(无描述)" : p.description));
        }

        bool ok = false;
        QString sel = QInputDialog::getItem(this, RVDLTR("插件列表"),
                                            RVDLTR("已加载插件（选择并确定以执行）："),
                                            names, 0, false, &ok);
        if (!ok || sel.isEmpty())
            return;
        // 从显示文本中提取插件名（"name  v版本  —  描述" 的第一个空格字段）
        QString name = sel.section(' ', 0, 0).trimmed();
        execute_plugin(name.toStdString());
    }

    void MainWindow::execute_plugin(const std::string &name)
    {
        const auto *p = plugin_manager_.find(name);
        if (!p)
        {
            QMessageBox::warning(this, RVDLTR("执行插件"), RVDLTR("未找到插件：%1").arg(QString::fromStdString(name)));
            return;
        }

        // ── 插件 rvdl 源码写入临时文件 ───────────────────────────
        QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/rvdl_plugin_XXXXXX.rvdl"));
        tmp.setAutoRemove(true);
        if (!tmp.open())
        {
            QMessageBox::warning(this, RVDLTR("执行插件"), RVDLTR("无法创建临时文件。"));
            return;
        }
        tmp.write(p->rvdl_source.c_str(), static_cast<qint64>(p->rvdl_source.size()));
        tmp.flush();

        // ── 定位 server/out/cli.js（rvdl → IR 前端）──────────────
        QDir dir(QCoreApplication::applicationDirPath());
        QStringList candidates = {
            dir.absoluteFilePath("../../../server/out/cli.js"),
            dir.absoluteFilePath("../../../../server/out/cli.js"),
            dir.absoluteFilePath("server/out/cli.js"),
        };
        QString cli;
        for (const QString &c : candidates)
            if (QFile::exists(c))
            {
                cli = c;
                break;
            }
        if (cli.isEmpty())
        {
            QMessageBox::warning(this, RVDLTR("执行插件"), RVDLTR("未找到 server/out/cli.js（rvdl 编译器前端）。"));
            return;
        }

        // ── 运行 node cli.js ir <tmp.rvdl> 生成 LLVM IR ───────────
        QProcess proc;
        proc.start("node", {cli, QStringLiteral("ir"), tmp.fileName()});
        if (!proc.waitForStarted(3000) || !proc.waitForFinished(15000))
        {
            QMessageBox::warning(this, RVDLTR("执行插件"), RVDLTR("rvdl 编译器前端运行失败。"));
            return;
        }
        QByteArray ir = proc.readAllStandardOutput();
        QByteArray err = proc.readAllStandardError();
        if (ir.trimmed().isEmpty())
        {
            QMessageBox::warning(this, RVDLTR("执行插件"),
                                 RVDLTR("rvdl 编译失败：\n%1").arg(QString::fromUtf8(err)));
            return;
        }

        // ── embedded JIT 编译并执行入口函数 ─────────────────────
        editor::QkEditor editor;
        editor.set_backend(editor::Backend::Embedded);
        editor.set_text(ir.toStdString());
        auto res = editor.compile_and_run(p->entry);
        QString msg = QString::fromStdString(res.output.empty() ? res.message : res.output);
        QMessageBox::information(this, RVDLTR("执行插件"),
                                 RVDLTR("插件 %1 执行结果：\n%2")
                                     .arg(QString::fromStdString(name), msg));
    }

    void MainWindow::closeEvent(QCloseEvent *event)
    {
        // 检查资产属性面板是否有未保存的编辑
        if (asset_details_ && asset_details_->is_dirty())
        {
            const QMessageBox::StandardButton ret = QMessageBox::warning(
                this, RVDLTR("未保存的更改"),
                RVDLTR("资产 %1 有未保存的更改。").arg(asset_details_->current_path()),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                QMessageBox::Save);

            if (ret == QMessageBox::Save)
            {
                asset_details_->save();
                if (asset_details_->is_dirty())
                {
                    event->ignore(); // 保存失败，阻止关闭
                    return;
                }
            }
            else if (ret == QMessageBox::Cancel)
            {
                event->ignore();
                return;
            }
            // Discard → 继续关闭
        }

        // 保存窗口状态（几何 + dock 布局）
        QSettings settings;
        settings.setValue("mainwindow/geometry", saveGeometry());
        settings.setValue("mainwindow/state", saveState());

        event->accept();
    }
}