#include "project_browser.h"
#include "project_store.h"
#include "theme_manager.h"
#include "ui_utils.h"
#include "i18n/i18n.h"

#include <QApplication>
#include <QCursor>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QButtonGroup>
#include <QDateTime>
#include <QFileInfo>
#include <QMessageBox>

namespace rvdlrsp::gui
{

    // 卡片式可选按钮（返回按钮，由调用方加入布局）
    static QPushButton *addCard(QButtonGroup *group, const QString &text, int id)
    {
        auto *btn = new QPushButton(text);
        btn->setCheckable(true);
        btn->setMinimumHeight(52);
        btn->setProperty("cardButton", true); // 选中态样式由全局主题 QSS 提供
        group->addButton(btn, id);
        return btn;
    }

    ProjectBrowser::ProjectBrowser(QWidget *parent) : QWidget(parent)
    {
        setWindowTitle(RVDLTR("Ravendrel RSP — 工程浏览器"));
        resize(fit_screen(0.8, QSize(1080, 680)));
        buildUi();
        refreshProjectList();
    }

    void ProjectBrowser::buildUi()
    {
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        // ── 顶部栏：标题 + 主题切换按钮（右上角）────────────────────
        auto *top = new QHBoxLayout();
        top->setContentsMargins(12, 8, 12, 6);
        auto *title = new QLabel(RVDLTR("<b>Ravendrel RSP — 工程浏览器</b>"));
        top->addWidget(title);
        top->addStretch(1);
        theme_btn_ = new QPushButton();
        theme_btn_->setObjectName("themeToggle");
        theme_btn_->setCursor(Qt::PointingHandCursor);
        connect(theme_btn_, &QPushButton::clicked, this, &ProjectBrowser::onToggleTheme);
        top->addWidget(theme_btn_);
        applyThemeButton();
        outer->addLayout(top);

        auto *root = new QHBoxLayout();
        outer->addLayout(root, 1);

        // ── 左：历史任务列表 ──────────────────────────────────────────
        auto *left = new QVBoxLayout();
        left->addWidget(new QLabel(RVDLTR("<b>历史仿真任务</b>")));
        history_list_ = new QListWidget();
        left->addWidget(history_list_, 1);

        auto *btn_row = new QHBoxLayout();
        new_btn_ = new QPushButton(RVDLTR("新建"));
        open_btn_ = new QPushButton(RVDLTR("打开"));
        delete_btn_ = new QPushButton(RVDLTR("删除"));
        archive_btn_ = new QPushButton(RVDLTR("归档"));
        btn_row->addWidget(new_btn_);
        btn_row->addWidget(open_btn_);
        btn_row->addWidget(delete_btn_);
        btn_row->addWidget(archive_btn_);
        left->addLayout(btn_row);

        QWidget *left_w = new QWidget();
        left_w->setLayout(left);
        left_w->setFixedWidth(280);
        root->addWidget(left_w);

        // ── 右：新建任务向导 ──────────────────────────────────────────
        auto *form = new QVBoxLayout();

        auto *info = new QGroupBox(RVDLTR("项目信息"));
        auto *info_l = new QFormLayout(info);
        name_edit_ = new QLineEdit();
        info_l->addRow(RVDLTR("任务名称"), name_edit_);
        form->addWidget(info);

        auto *robot = new QGroupBox(RVDLTR("机器人类型"));
        auto *robot_l = new QHBoxLayout(robot);
        robot_group_ = new QButtonGroup(this);
        robot_group_->setExclusive(true);
        robot_l->addWidget(addCard(robot_group_, RVDLTR("机器臂"), (int)RobotType::Arm));
        robot_l->addWidget(addCard(robot_group_, RVDLTR("移动机器人"), (int)RobotType::Mobile));
        robot_l->addWidget(addCard(robot_group_, RVDLTR("无人机"), (int)RobotType::Drone));
        robot_l->addWidget(addCard(robot_group_, RVDLTR("人形机器人"), (int)RobotType::Humanoid));
        robot_l->addWidget(addCard(robot_group_, RVDLTR("自定义"), (int)RobotType::Custom));
        robot_group_->button((int)RobotType::Humanoid)->setChecked(true);
        form->addWidget(robot);

        auto *scene = new QGroupBox(RVDLTR("仿真场景模板"));
        auto *scene_l = new QGridLayout(scene);
        scene_group_ = new QButtonGroup(this);
        scene_group_->setExclusive(true);
        const SceneTemplate scene_types[6] = {
            SceneTemplate::Industrial, SceneTemplate::Home, SceneTemplate::Outdoor,
            SceneTemplate::GroundAtmosphere, SceneTemplate::SpaceStation, SceneTemplate::Custom};
        int r = 0, c = 0;
        for (SceneTemplate t : scene_types)
        {
            scene_l->addWidget(addCard(scene_group_, QString::fromStdString(display_name(t)), (int)t), r, c);
            if (++c == 3)
            {
                c = 0;
                ++r;
            }
        }
        scene_group_->button((int)SceneTemplate::Industrial)->setChecked(true);
        form->addWidget(scene);

        auto *params = new QGroupBox(RVDLTR("参数设置"));
        auto *params_l = new QFormLayout(params);
        version_edit_ = new QLineEdit("1.0.0");
        physics_combo_ = new QComboBox();
        physics_combo_->addItem("AlphaPHY 1.0", (int)PhysicsEngine::AlphaPHY_v1);
        physics_combo_->addItem("AlphaPHY 2.0", (int)PhysicsEngine::AlphaPHY_v2);
        backend_combo_ = new QComboBox();
        backend_combo_->addItem(QString::fromStdString(display_name(SimBackend::QVM)), (int)SimBackend::QVM);
        backend_combo_->addItem(QString::fromStdString(display_name(SimBackend::QM_Superconducting)), (int)SimBackend::QM_Superconducting);
        backend_combo_->addItem(QString::fromStdString(display_name(SimBackend::QM_TrappedIon)), (int)SimBackend::QM_TrappedIon);
        backend_combo_->addItem(QString::fromStdString(display_name(SimBackend::QM_NeutralAtom)), (int)SimBackend::QM_NeutralAtom);
        gpu_check_ = new QCheckBox(RVDLTR("启用 GPU 加速"));
        params_l->addRow(RVDLTR("仿真引擎版本"), version_edit_);
        params_l->addRow(RVDLTR("物理引擎"), physics_combo_);
        params_l->addRow(RVDLTR("仿真后端"), backend_combo_);
        params_l->addRow("", gpu_check_);
        form->addWidget(params);

        auto *ws = new QGroupBox(RVDLTR("工作区切换"));
        auto *ws_l = new QHBoxLayout(ws);
        workspace_group_ = new QButtonGroup(this);
        workspace_group_->setExclusive(true);
        ws_l->addWidget(addCard(workspace_group_, RVDLTR("仿真调试"), (int)Workspace::SimDebug));
        ws_l->addWidget(addCard(workspace_group_, RVDLTR("机器人控制"), (int)Workspace::RobotControl));
        ws_l->addWidget(addCard(workspace_group_, RVDLTR("场景编辑"), (int)Workspace::SceneEdit));
        workspace_group_->button((int)Workspace::SimDebug)->setChecked(true);
        form->addWidget(ws);

        form->addStretch(1);
        launch_btn_ = new QPushButton(RVDLTR("启动仿真 →"));
        launch_btn_->setMinimumHeight(44);
        form->addWidget(launch_btn_);

        QWidget *form_w = new QWidget();
        form_w->setLayout(form);
        root->addWidget(form_w, 1);

        connect(new_btn_, &QPushButton::clicked, this, &ProjectBrowser::onNewProject);
        connect(open_btn_, &QPushButton::clicked, this, &ProjectBrowser::onOpenProject);
        connect(delete_btn_, &QPushButton::clicked, this, &ProjectBrowser::onDeleteProject);
        connect(archive_btn_, &QPushButton::clicked, this, &ProjectBrowser::onArchiveProject);
        connect(launch_btn_, &QPushButton::clicked, this, &ProjectBrowser::onLaunch);
        connect(history_list_, &QListWidget::currentRowChanged, this, &ProjectBrowser::onProjectSelected);
    }

    void ProjectBrowser::refreshProjectList()
    {
        history_list_->blockSignals(true);
        history_list_->clear();

        // list_projects 已按最近修改时间倒序，首项即「最近使用」
        const auto projects = list_projects();
        for (size_t i = 0; i < projects.size(); ++i)
        {
            const QString path = QString::fromStdString(projects[i]);
            const QFileInfo fi(path);
            const QString name = fi.completeBaseName(); // 去掉 .qrsp.json
            const QString mtime = fi.lastModified().toString("yyyy-MM-dd hh:mm");

            auto *item = new QListWidgetItem();
            item->setText(i == 0 ? QString("%1  ★ %2").arg(name, mtime)
                                 : QString("%1     %2").arg(name, mtime));
            item->setToolTip(path);
            item->setData(Qt::UserRole, path); // 完整路径供加载/删除/归档使用
            history_list_->addItem(item);
        }
        history_list_->blockSignals(false);
    }

    SimulationConfig ProjectBrowser::collectConfig() const
    {
        SimulationConfig cfg;
        cfg.project_name = name_edit_->text().trimmed().toStdString();
        if (cfg.project_name.empty())
            cfg.project_name = "Simulation_" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss").toStdString();
        cfg.project_path = project_path_for(cfg.project_name);
        cfg.robot_type = (RobotType)robot_group_->checkedId();
        cfg.scene_template = (SceneTemplate)scene_group_->checkedId();
        cfg.workspace = (Workspace)workspace_group_->checkedId();
        cfg.backend = (SimBackend)backend_combo_->currentData().toInt();
        cfg.physics_engine = (PhysicsEngine)physics_combo_->currentData().toInt();
        cfg.gpu_accel = gpu_check_->isChecked();
        cfg.engine_version = version_edit_->text().toStdString();

        // 场景模板 → 环境参数（温度/风速/重力/灯光）
        switch (cfg.scene_template)
        {
        case SceneTemplate::Home:
            cfg.env.temperature_c = 22.0;
            cfg.env.wind_speed = 0.0;
            cfg.env.gravity = -9.81;
            cfg.env.sun_dir[0] = 0.2f;
            cfg.env.sun_dir[1] = -0.8f;
            cfg.env.sun_dir[2] = 0.5f;
            cfg.env.sun_intensity = 1.0f;
            break;
        case SceneTemplate::Outdoor:
            cfg.env.temperature_c = 28.0;
            cfg.env.wind_speed = 3.5;
            cfg.env.gravity = -9.81;
            cfg.env.sun_intensity = 1.6f;
            break;
        case SceneTemplate::GroundAtmosphere:
            cfg.env.temperature_c = 15.0;
            cfg.env.wind_speed = 8.0;
            cfg.env.gravity = -9.81;
            cfg.env.sun_dir[1] = -0.3f;
            cfg.env.sun_intensity = 1.4f;
            break;
        case SceneTemplate::SpaceStation:
            cfg.env.temperature_c = -120.0;
            cfg.env.wind_speed = 0.0;
            cfg.env.gravity = -0.02; // 微重力
            cfg.env.sun_intensity = 1.8f;
            break;
        case SceneTemplate::Custom:
        case SceneTemplate::Industrial:
        default:
            cfg.env.temperature_c = 22.0;
            cfg.env.wind_speed = 0.0;
            cfg.env.gravity = -9.81;
            break;
        }
        return cfg;
    }

    void ProjectBrowser::applyConfig(const SimulationConfig &cfg)
    {
        name_edit_->setText(QString::fromStdString(cfg.project_name));
        version_edit_->setText(QString::fromStdString(cfg.engine_version));
        gpu_check_->setChecked(cfg.gpu_accel);
        robot_group_->button((int)cfg.robot_type)->setChecked(true);
        scene_group_->button((int)cfg.scene_template)->setChecked(true);
        workspace_group_->button((int)cfg.workspace)->setChecked(true);
        int bi = backend_combo_->findData((int)cfg.backend);
        if (bi >= 0)
            backend_combo_->setCurrentIndex(bi);
        int pi = physics_combo_->findData((int)cfg.physics_engine);
        if (pi >= 0)
            physics_combo_->setCurrentIndex(pi);
    }

    void ProjectBrowser::onNewProject()
    {
        name_edit_->setText("Simulation_" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        history_list_->clearSelection();
    }

    void ProjectBrowser::onOpenProject()
    {
        auto *item = history_list_->currentItem();
        if (!item)
            return;
        SimulationConfig cfg;
        if (load_project(item->data(Qt::UserRole).toString().toStdString(), cfg))
            applyConfig(cfg);
    }

    void ProjectBrowser::onDeleteProject()
    {
        auto *item = history_list_->currentItem();
        if (!item)
            return;
        const QString path = item->data(Qt::UserRole).toString();
        if (QMessageBox::question(this, RVDLTR("删除项目"),
                                  RVDLTR("确定删除项目 %1 吗？此操作不可撤销。").arg(path),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
        if (!delete_project(path.toStdString()))
            QMessageBox::warning(this, RVDLTR("删除项目"), RVDLTR("删除失败：%1").arg(path));
        refreshProjectList();
    }

    void ProjectBrowser::onArchiveProject()
    {
        auto *item = history_list_->currentItem();
        if (!item)
            return;
        const QString path = item->data(Qt::UserRole).toString();
        if (!archive_project(path.toStdString()))
            QMessageBox::warning(this, RVDLTR("归档项目"), RVDLTR("归档失败：%1").arg(path));
        refreshProjectList();
    }

    void ProjectBrowser::onProjectSelected(int /*row*/)
    {
        auto *item = history_list_->currentItem();
        if (!item)
            return;
        SimulationConfig cfg;
        if (load_project(item->data(Qt::UserRole).toString().toStdString(), cfg))
            applyConfig(cfg);
    }

    void ProjectBrowser::onLaunch()
    {
        SimulationConfig cfg = collectConfig();
        save_project(cfg);
        emit launchRequested(cfg);
    }

    void ProjectBrowser::onToggleTheme()
    {
        ThemeManager::instance().toggle();
        ThemeManager::instance().apply(*qApp);
        applyThemeButton();
    }

    void ProjectBrowser::applyThemeButton()
    {
        if (theme_btn_)
            theme_btn_->setText(ThemeManager::toggleText(ThemeManager::instance().current()));
    }
}