#include "asset_viewers.h"
#include "content_browser.h"
#include "i18n/i18n.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QScrollArea>
#include <QPixmap>

#include "render/mesh_loader.hpp"

namespace rvdlrsp::gui
{

    // ─── 音频播放器 ──────────────────────────────────────────────
    AudioPlayerWindow::AudioPlayerWindow(const QString &path, QWidget *parent)
        : QWidget(parent)
    {
        setWindowTitle(RVDLTR("音频播放 — %1").arg(QFileInfo(path).fileName()));
        resize(420, 140);
        setAttribute(Qt::WA_DeleteOnClose);

        auto *lay = new QVBoxLayout(this);

        status_label_ = new QLabel(QFileInfo(path).fileName());
        status_label_->setStyleSheet("font-size:14px;font-weight:bold;");
        lay->addWidget(status_label_);

        seek_slider_ = new QSlider(Qt::Horizontal);
        seek_slider_->setRange(0, 100);
        lay->addWidget(seek_slider_);

        auto *row = new QHBoxLayout();
        auto *play_btn = new QPushButton(RVDLTR("播放 / 暂停"));
        play_btn->setFixedHeight(34);
        row->addWidget(play_btn);
        row->addStretch(1);
        lay->addLayout(row);

        player_ = new QMediaPlayer(this);
        audio_ = new QAudioOutput(this);
        player_->setAudioOutput(audio_);
        player_->setSource(QUrl::fromLocalFile(path));

        connect(play_btn, &QPushButton::clicked, this, &AudioPlayerWindow::toggle_play);
        connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 pos)
                {
        if (player_->duration() > 0)
            seek_slider_->setValue(static_cast<int>(pos * 100 / player_->duration())); });
        connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64 dur)
                {
        seek_slider_->setRange(0, 100);
        Q_UNUSED(dur); });
        connect(player_, &QMediaPlayer::playbackStateChanged, this, [this, path](QMediaPlayer::PlaybackState st)
                {
        playing_ = (st == QMediaPlayer::PlayingState);
        status_label_->setText(QString("%1 — %2")
            .arg(QFileInfo(path).fileName())
            .arg(playing_ ? RVDLTR("播放中") : RVDLTR("已暂停"))); });
        connect(seek_slider_, &QSlider::sliderMoved, this, [this](int v)
                {
        if (player_->duration() > 0)
            player_->setPosition(static_cast<qint64>(v * player_->duration() / 100)); });
    }

    AudioPlayerWindow::~AudioPlayerWindow() = default;

    void AudioPlayerWindow::toggle_play()
    {
        if (!player_)
            return;
        if (playing_)
            player_->pause();
        else
            player_->play();
    }

    // ─── 图片预览 ────────────────────────────────────────────────
    ImageViewerWindow::ImageViewerWindow(const QString &path, QWidget *parent)
        : QWidget(parent)
    {
        setWindowTitle(RVDLTR("图片预览 — %1").arg(QFileInfo(path).fileName()));
        resize(800, 600);
        setAttribute(Qt::WA_DeleteOnClose);

        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        image_label_ = new QLabel();
        image_label_->setAlignment(Qt::AlignCenter);
        scroll->setWidget(image_label_);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(scroll);

        QPixmap pm(path);
        if (!pm.isNull())
        {
            image_label_->setPixmap(pm);
            image_label_->setMinimumSize(1, 1);
        }
        else
        {
            image_label_->setText(RVDLTR("无法加载图片"));
        }
    }

    // ─── 模型查看器 ──────────────────────────────────────────────
    ModelViewerWindow::ModelViewerWindow(const QString &path, QWidget *parent)
        : QWidget(parent)
    {
        setWindowTitle(RVDLTR("模型查看器 — %1").arg(QFileInfo(path).fileName()));
        resize(640, 720);
        setAttribute(Qt::WA_DeleteOnClose);

        auto *lay = new QVBoxLayout(this);

        info_label_ = new QLabel();
        lay->addWidget(info_label_);

        preview_label_ = new QLabel();
        preview_label_->setAlignment(Qt::AlignCenter);
        // 缩略图本身是深色渲染，深色底在两种主题下均协调
        preview_label_->setStyleSheet("background:#14171c;");
        lay->addWidget(preview_label_, 1);

        // 加载网格并离屏渲染大图
        try
        {
            render::Mesh mesh = render::MeshLoader::load(path.toStdString());
            info_label_->setText(RVDLTR("顶点: %1   三角形: %2")
                                     .arg(mesh.vertices.size())
                                     .arg(mesh.indices.size() / 3));

            QImage img = ThumbnailRenderer::rasterize_mesh(mesh, 512);
            if (!img.isNull())
                preview_label_->setPixmap(QPixmap::fromImage(img));
            else
                preview_label_->setText(RVDLTR("（无法渲染模型）"));
        }
        catch (const std::exception &e)
        {
            info_label_->setText(RVDLTR("加载失败: %1").arg(e.what()));
            preview_label_->setText(RVDLTR("（模型加载失败）"));
        }
    }
}