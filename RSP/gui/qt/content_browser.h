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

    // ─── 资产状态角标（位标志，可叠加）──────────────────────────
    namespace AssetStatus
    {
        enum Flag
        {
            Clean = 0,
            Dirty = 1 << 0,      // * 已修改未保存
            CheckedOut = 1 << 1, // ✓ 版本控制已检出
            Added = 1 << 2,      // + 版本控制新增
        };
    }

    struct AssetItem
    {
        QString name;
        QString path; // 绝对路径
        AssetType type = AssetType::Unknown;
        int status = AssetStatus::Clean;
    };

    // ─── 版本控制（git 真实对接）────────────────────────────────
    class VcsClient
    {
    public:
        explicit VcsClient(const QString &repoRoot);
        // 刷新一次 git status，返回 path → status 映射（仅含仓库内文件）
        QHash<QString, int> refresh();

    private:
        QString repo_root_;
    };

    // ─── 资产状态存储（dirty 跟踪 + VCS 状态合并）────────────────
    class AssetStore : public QObject
    {
        Q_OBJECT
    public:
        explicit AssetStore(const QString &projectRoot, QObject *parent = nullptr);

        void set_project_root(const QString &root);
        QString project_root() const { return root_; }

        // dirty（内存中已修改未保存）
        void mark_dirty(const QString &path);
        void mark_clean(const QString &path);
        bool is_dirty(const QString &path) const;

        // 查询某资产合并后的状态（dirty + VCS）
        int status_of(const QString &path) const;
        // 刷新 VCS 状态（异步执行 git，完成后 emit statusesChanged）
        void refresh_vcs();

    signals:
        void statusesChanged();

    private:
        QString root_;
        QSet<QString> dirty_;
        QHash<QString, int> vcs_status_;
        VcsClient vcs_client_;
    };

    // ─── 资产列表模型（缩略图网格用）────────────────────────────
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

    // ─── 缩略图渲染（纯函数，线程安全，返回 QImage）──────────────
    class ThumbnailRenderer
    {
    public:
        static const int ThumbSize = 144;

        // 按类型分派渲染，返回缩略图（失败返回类型图标）
        static QImage render(const QString &path, AssetType type);
        static QImage render_mesh(const QString &path);
        static QImage render_texture(const QString &path);
        static QImage render_type_icon(AssetType type);
        // 软件光栅化 render::Mesh → QImage
        static QImage rasterize_mesh(const render::Mesh &mesh, int size);
    };

    // ─── 异步缩略图加载器 ──────────────
    class AsyncThumbnailLoader : public QObject
    {
        Q_OBJECT
    public:
        explicit AsyncThumbnailLoader(QObject *parent = nullptr);
        ~AsyncThumbnailLoader() override;

        // 主线程调用：返回已缓存缩略图或占位图；未缓存则投递后台渲染。
        QImage thumbnail(const QString &path, AssetType type);
        void clear_cache();
        void set_visible(const QSet<QString> &paths);
        void cancel_all(); // 取消全部在途任务并重置进度
        // 主线程调用：进度统计（用于进度条百分比）
        int pending_count() const { std::lock_guard<std::mutex> lk(mutex_); return static_cast<int>(pending_.size()); }
        int total_submitted() const { std::lock_guard<std::mutex> lk(mutex_); return total_submitted_; } // 本批已投递总数
        int total_completed() const { std::lock_guard<std::mutex> lk(mutex_); return total_completed_; } // 本批已完成总数

    signals:
        void thumbnailReady(const QString &path); // 某资产缩略图渲染完成
        void progressChanged();                   // 在途任务数变化（进度显示）

    private:
        struct Request
        {
            QString path;
            AssetType type;
        };
        void workerLoop();
        void drainResults();

        static const size_t kMaxInFlight = 8; // 并发上限：最多同时在途的缩略图任务

        QHash<QString, QImage> cache_;
        QSet<QString> pending_;
        QSet<QString> visible_;   // 可视区资产（替换式）
        int total_submitted_ = 0; // 本批投递总数
        int total_completed_ = 0; // 本批完成总数

        // 保护跨线程共享状态（queue_/results_/visible_/stop_），
        // 同时为主线程独占状态（cache_/pending_/total_*）提供防御性加锁
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<Request> queue_;                      // worker 消费
        std::deque<std::pair<QString, QImage>> results_; // worker 产出
        std::thread worker_;
        bool stop_ = false;
    };

    // ─── 缩略图 + 角标 绘制代理 ─────────────────────────────────
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

    // ─── 内容浏览器 ─────────────────────────────────
    class ContentBrowser : public QWidget
    {
        Q_OBJECT
    public:
        explicit ContentBrowser(const QString &projectRoot, QWidget *parent = nullptr);

        void set_project_root(const QString &root);

        // 供外部（如资产属性面板）标记/清除 dirty 状态
        void set_dirty(const QString &path, bool dirty);
        AssetStore *store() const { return store_; }

        // 视图模式
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
        void assetActivated(const QString &path); // 右键"打开"→ 打开文件
        void assetSelected(const QString &path);  // 双击/单击选中 → 显示属性

    private:
        void build_ui();
        void navigate_to(const QString &dir);
        void refresh_current_dir();
        void update_visible_set(); // 重算视口内可见资产并同步给缩略图加载器
        void update_progress();    // 刷新缩略图渲染进度标签
        void show_context_menu(const QPoint &pos);
        void create_asset(AssetType type);
        void paste_from_clipboard();

        QString context_path_; // 右键点击处的资产路径（"打开"用）

        QString root_; // 项目根目录（Content 目录）
        QString current_dir_;

        QFileSystemModel *fs_model_ = nullptr;
        QTreeView *tree_ = nullptr;
        QStackedWidget *stack_ = nullptr; // 图标列表 / 详情表格 切换
        QListView *list_ = nullptr;
        QTreeView *detail_view_ = nullptr; // 详细信息：多列表格视图
        QFileSystemModel *detail_model_ = nullptr;
        QLabel *path_label_ = nullptr;
        QProgressBar *progress_bar_ = nullptr; // 缩略图渲染进度条
        QPushButton *cancel_btn_ = nullptr;    // 取消缩略图渲染
        QLineEdit *filter_ = nullptr;
        QPushButton *back_btn_ = nullptr;

        AssetModel *model_ = nullptr;
        AsyncThumbnailLoader *loader_ = nullptr;
        AssetStore *store_ = nullptr;
        ViewMode view_mode_ = ViewMode::LargeIcons;
    };
}