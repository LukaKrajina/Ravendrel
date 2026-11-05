#include "loading_screen.h"
#include "simulation_host.hpp"
#include "ui_utils.h"
#include "i18n/i18n.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>

namespace rvdlrsp::gui
{

    const char *LoadingScreen::step_names_[StepCount] = {
        "加载机器人硬件/模拟模型",
        "初始化物理引擎",
        "加载仿真场景环境（地形/障碍物/灯光/温度/风速）",
        "初始化量子设备并尝试连接",
        "初始化脑量子接口",
        "初始化量子学习机",
        "初始化机器人控制接口",
        "进入正式仿真主面"};

    LoadingScreen::LoadingScreen(const SimulationConfig &cfg, QWidget *parent)
        : QWidget(parent), cfg_(cfg)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setWindowTitle(RVDLTR("正在启动仿真任务"));
        resize(fit_screen(0.5, QSize(640, 440)));
        // 背景色/文字色由全局主题 QSS 提供（跟随白天/夜间模式）

        auto *root = new QVBoxLayout(this);
        root->addStretch(1);

        title_ = new QLabel(RVDLTR("Ravendrel RSP — 正在启动仿真任务"));
        title_->setAlignment(Qt::AlignCenter);
        title_->setStyleSheet("font-size:20px;font-weight:bold;");
        root->addWidget(title_);

        auto *proj = new QLabel(RVDLTR("项目：%1").arg(QString::fromStdString(cfg_.project_name)));
        proj->setAlignment(Qt::AlignCenter);
        proj->setStyleSheet("color:gray;");
        root->addWidget(proj);

        total_bar_ = new QProgressBar();
        total_bar_->setRange(0, 100);
        total_bar_->setValue(0);
        total_bar_->setFixedHeight(18);
        total_bar_->setTextVisible(false);
        root->addWidget(total_bar_);

        step_label_ = new QLabel();
        step_label_->setStyleSheet("color:gray;");
        root->addWidget(step_label_);

        step_bar_ = new QProgressBar();
        step_bar_->setRange(0, 100);
        step_bar_->setValue(0);
        step_bar_->setFixedHeight(8);
        step_bar_->setTextVisible(false);
        root->addWidget(step_bar_);

        root->addSpacing(12);
        for (int i = 0; i < StepCount; ++i)
        {
            auto *row = new QLabel(QString("[ ] %1").arg(I18n::instance().tr(QString::fromUtf8(step_names_[i]))));
            row->setStyleSheet("color:gray;");
            step_rows_.push_back(row);
            root->addWidget(row);
        }
        root->addStretch(1);

        timer_ = new QTimer(this);
        timer_->setInterval(25);
        connect(timer_, &QTimer::timeout, this, &LoadingScreen::onTick);
    }

    LoadingScreen::~LoadingScreen() = default;

    void LoadingScreen::start()
    {
        host_ = new SimulationHost(cfg_);
        current_step_ = 0;
        beginStep(0);
        timer_->start();
    }

    void LoadingScreen::setStepState(int index, const QString &mark, const QString &state)
    {
        (void)state;
        if (index < 0 || index >= step_rows_.size())
            return;
        step_rows_[index]->setText(QString("[%1] %2").arg(mark, I18n::instance().tr(QString::fromUtf8(step_names_[index]))));
        if (mark == "✓")
            step_rows_[index]->setStyleSheet("color:#4caf50;");
        else if (mark == "▶")
            step_rows_[index]->setStyleSheet("color:#3aa0ff;font-weight:bold;");
        else if (mark == "✗")
            step_rows_[index]->setStyleSheet("color:#f44336;");
    }

    void LoadingScreen::beginStep(int index)
    {
        step_label_->setText(I18n::instance().tr(QString::fromUtf8(step_names_[index])));
        step_bar_->setValue(0);
        setStepState(index, "▶", "进行中…");

        bool ok = true;
        switch (index)
        {
        case 0:
            ok = host_->loadRobotModel();
            break;
        case 1:
            ok = host_->initPhysics();
            break;
        case 2:
            ok = host_->loadSceneEnvironment();
            break;
        case 3:
            ok = host_->initQuantumDevice();
            break;
        case 4:
            ok = host_->initBrainInterface();
            break;
        case 5:
            ok = host_->initQuantumLearningMachine();
            break;
        case 6:
            ok = host_->initRobotControl();
            break;
        case 7:
        default:
            ok = true;
            break;
        }

        if (!ok)
        {
            timer_->stop();
            setStepState(index, "✗", "失败");
            emit failed(I18n::instance().tr(QString::fromUtf8(step_names_[index])), RVDLTR("初始化失败"));
        }
    }

    void LoadingScreen::onTick()
    {
        int v = step_bar_->value() + 8; // 动画速度
        if (v < 100)
        {
            step_bar_->setValue(v);
            return;
        }

        step_bar_->setValue(100);
        setStepState(current_step_, "✓", "完成");
        total_bar_->setValue((current_step_ + 1) * 100 / StepCount);

        if (current_step_ >= StepCount - 1)
        {
            timer_->stop();
            setStepState(StepCount - 1, "✓", "完成");
            SimulationHost *h = host_;
            host_ = nullptr;
            emit finished(h);
            return;
        }
        ++current_step_;
        beginStep(current_step_);
    }
}