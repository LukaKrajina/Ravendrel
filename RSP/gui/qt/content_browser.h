#pragma once
#include <QWidget>
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QString>
#include <QImage>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "render/scene.hpp"
#include "asset_types.h"

class QTreeView;
class QFileSystemModel;
class QListView;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QStackedWidget;

namespace rvdlrsp::gui
{

    namespace AssetStatus
    {
        enum Flag
        {
            Clean = 0,
            Dirty = 1 << 0,
            CheckedOut = 1 << 1,
            Added = 1 << 2,
        };
    }

    struct AssetItem
    {
        QString name;
        QString path;
        AssetType type = AssetType::Unknown;
        int status = AssetStatus::Clean;
    };

    class VcsClient
    {
    public:
        explicit VcsClient(const QString &repoRoot);
        QHash<QString, int> refresh();

    private:
        QString repo_root_;
    };

    class AssetStore : public QObject
    {
        Q_OBJECT
    public:
        explicit AssetStore(const QString &projectRoot, QObject *parent = nullptr);

        void set_project_root(const QString &root);
        QString project_root() const { return root_; }

        void mark_dirty(const QString &path);
        void mark_clean(const QString &path);
        bool is_dirty(const QString &path) const;

        int status_of(const QString &path) const;
        void refresh_vcs();

    signals:
        void statusesChanged();

    private:
        QString root_;
        QSet<QString> dirty_;
        QHash<QString, int> vcs_status_;
        VcsClient vcs_client_;
    };

    class AssetModel : public QAbstractListModel
    {
        Q_OBJECT
    public:
        enum Role
        {
            TypeRole = Qt::UserRole + 1,
            StatusRole,
            PathRole
        };

        explicit AssetModel(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        Qt::ItemFlags flags(const QModelIndex &index) const override;
        QStringList mimeTypes() const override;
        QMimeData *mimeData(const QModelIndexList &indexes) const override;

        void set_items(const QVector<AssetItem> &items);
        const AssetItem &item_at(int row) const;

    private:
        QVector<AssetItem> items_;
    };

    class ThumbnailRenderer
    {
    public:
        static const int ThumbSize = 144;

        static QImage render(const QString &path, AssetType type);
        static QImage render_mesh(const QString &path);
        static QImage render_texture(const QString &path);
        static QImage render_type_icon(AssetType type);
        static QImage rasterize_mesh(const render::Mesh &mesh, int size);
    };

    class AsyncThumbnailLoader : public QObject
    {
        Q_OBJECT
    public:
        explicit AsyncThumbnailLoader(QObject *parent = nullptr);
        ~AsyncThumbnailLoader() override;

        QImage thumbnail(const QString &path, AssetType type);
        void clear_cache();
        void set_visible(const QSet<QString> &paths);
        void cancel_all();
        int pending_count() const { std::lock_guard<std::mutex> lk(mutex_); return static_cast<int>(pending_.size()); }
        int total_submitted() const { std::lock_guard<std::mutex> lk(mutex_); return total_submitted_; }
        int total_completed() const { std::lock_guard<std::mutex> lk(mutex_); return total_completed_; }

    signals:
        void thumbnailReady(const QString &path);
        void progressChanged();

    private:
        struct Request
        {
            QString path;
            AssetType type;
        };
        void workerLoop();
        void drainResults();

        static const size_t kMaxInFlight = 8;

        QHash<QString, QImage> cache_;
        QSet<QString> pending_;
        QSet<QString> visible_;
        int total_submitted_ = 0;
        int total_completed_ = 0;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<Request> queue_;
        std::deque<std::pair<QString, QImage>> results_;
        std::thread worker_;
        bool stop_ = false;
    };

    class ThumbnailDelegate : public QStyledItemDelegate
    {
    public:
        explicit ThumbnailDelegate(AsyncThumbnailLoader *loader, QObject *parent = nullptr);

        void paint(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
        QSize sizeHint(const QStyleOptionViewItem &option,
                       const QModelIndex &index) const override;

    private:
        void draw_badge(QPainter *p, const QRect &thumb_rect, int status) const;
        AsyncThumbnailLoader *loader_ = nullptr;
    };

    class ContentBrowser : public QWidget
    {
        Q_OBJECT
    public:
        explicit ContentBrowser(const QString &projectRoot, QWidget *parent = nullptr);

        void set_project_root(const QString &root);

        void set_dirty(const QString &path, bool dirty);
        AssetStore *store() const { return store_; }

        enum class ViewMode
        {
            ExtraLargeIcons,
            LargeIcons,
            List,
            Details
        };
        void set_view_mode(ViewMode mode);
        ViewMode view_mode() const { return view_mode_; }

    signals:
        void assetActivated(const QString &path);
        void assetSelected(const QString &path);

    private:
        void build_ui();
        void navigate_to(const QString &dir);
        void refresh_current_dir();
        void update_visible_set();
        void update_progress();
        void show_context_menu(const QPoint &pos);
        void create_asset(AssetType type);
        void paste_from_clipboard();

        QString context_path_;

        QString root_;
        QString current_dir_;

        QFileSystemModel *fs_model_ = nullptr;
        QTreeView *tree_ = nullptr;
        QStackedWidget *stack_ = nullptr;
        QListView *list_ = nullptr;
        QTreeView *detail_view_ = nullptr;
        QFileSystemModel *detail_model_ = nullptr;
        QLabel *path_label_ = nullptr;
        QProgressBar *progress_bar_ = nullptr;
        QPushButton *cancel_btn_ = nullptr;
        QLineEdit *filter_ = nullptr;
        QPushButton *back_btn_ = nullptr;

        AssetModel *model_ = nullptr;
        AsyncThumbnailLoader *loader_ = nullptr;
        AssetStore *store_ = nullptr;
        ViewMode view_mode_ = ViewMode::LargeIcons;
    };
}