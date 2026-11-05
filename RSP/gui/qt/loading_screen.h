#pragma once
#include <QWidget>
#include <QVector>
#include "simulation_config.h"

class QProgressBar;
class QLabel;
class QTimer;

namespace rvdlrsp::gui
{

    class SimulationHost;

    class LoadingScreen : public QWidget
    {
        Q_OBJECT
    public:
        explicit LoadingScreen(const SimulationConfig &cfg, QWidget *parent = nullptr);
        ~LoadingScreen() override;

        void start();

    signals:
        void finished(SimulationHost *host); // 所有权转交给 MainWindow
        void failed(const QString &step, const QString &reason);
        void cancelled();

    private slots:
        void onTick();

    private:
        void beginStep(int index);
        void setStepState(int index, const QString &mark, const QString &state);

        static const int StepCount = 8;
        static const char *step_names_[StepCount];

        SimulationConfig cfg_;
        SimulationHost *host_ = nullptr;
        int current_step_ = 0;

        QLabel *title_ = nullptr;
        QLabel *step_label_ = nullptr;
        QProgressBar *total_bar_ = nullptr;
        QProgressBar *step_bar_ = nullptr;
        QVector<QLabel *> step_rows_;
        QTimer *timer_ = nullptr;
    };
}