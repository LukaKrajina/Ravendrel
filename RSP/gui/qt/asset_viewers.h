#pragma once
#include <QWidget>
#include <QImage>

class QLabel;
class QSlider;
class QMediaPlayer;
class QAudioOutput;

namespace rvdlrsp::gui {
    class AudioPlayerWindow : public QWidget {
        Q_OBJECT
    public:
        AudioPlayerWindow(const QString &path, QWidget *parent = nullptr);
        ~AudioPlayerWindow() override;

    private:
        void toggle_play();

        QMediaPlayer *player_ = nullptr;
        QAudioOutput *audio_ = nullptr;
        QLabel *status_label_ = nullptr;
        QSlider *seek_slider_ = nullptr;
        bool playing_ = false;
    };

    class ImageViewerWindow : public QWidget {
        Q_OBJECT
    public:
        ImageViewerWindow(const QString &path, QWidget *parent = nullptr);

    private:
        QLabel *image_label_ = nullptr;
    };

    class ModelViewerWindow : public QWidget {
        Q_OBJECT
    public:
        ModelViewerWindow(const QString &path, QWidget *parent = nullptr);

    private:
        QLabel *preview_label_ = nullptr;
        QLabel *info_label_ = nullptr;
    };
}