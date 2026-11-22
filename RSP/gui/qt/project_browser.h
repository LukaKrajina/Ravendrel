#pragma once
#include <QWidget>
#include "simulation_config.h"

class QListWidget;
class QPushButton;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QButtonGroup;

namespace rvdlrsp::gui
{

    class ProjectBrowser : public QWidget
    {
        Q_OBJECT
    public:
        explicit ProjectBrowser(QWidget *parent = nullptr);

    signals:
        void launchRequested(const SimulationConfig &config);

    private slots:
        void onNewProject();
        void onOpenProject();
        void onDeleteProject();
        void onArchiveProject();
        void onLaunch();
        void onProjectSelected(int row);
        void onToggleTheme();

    private:
        void buildUi();
        void refreshProjectList();
        void applyThemeButton();
        SimulationConfig collectConfig() const;
        void applyConfig(const SimulationConfig &cfg);

        QListWidget *history_list_ = nullptr;
        QPushButton *new_btn_, *open_btn_, *delete_btn_, *archive_btn_, *launch_btn_;
        QPushButton *theme_btn_ = nullptr;

        QButtonGroup *robot_group_ = nullptr;
        QButtonGroup *scene_group_ = nullptr;
        QButtonGroup *workspace_group_ = nullptr;

        QLineEdit *name_edit_ = nullptr;
        QLineEdit *version_edit_ = nullptr;
        QComboBox *physics_combo_ = nullptr;
        QComboBox *backend_combo_ = nullptr;
        QCheckBox *gpu_check_ = nullptr;
    };
}