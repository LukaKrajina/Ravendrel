#include "content_browser.h"
#include "theme_manager.h"
#include "i18n/i18n.h"

#include <QTreeView>
#include <QFileSystemModel>
#include <QListView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QSet>
#include <QMetaObject>
#include <QScrollBar>
#include <QMenu>
#include <QInputDialog>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QStackedWidget>
#include <QHeaderView>
#include <QDateTime>
#include <cmath>
#include <algorithm>

#include "render/mesh_loader.hpp"
#include "render/mat4.hpp"

namespace rvdlrsp::gui
{

    // ─── VCS（git）────────────────────────────────────────────────
    VcsClient::VcsClient(const QString &repoRoot) : repo_root_(repoRoot) {}

    QHash<QString, int> VcsClient::refresh()
    {
        QHash<QString, int> result;
        if (repo_root_.isEmpty() || !QDir(repo_root_).exists())
            return result;

        QProcess proc;
        proc.setWorkingDirectory(repo_root_);
        proc.setProgram("git");
        proc.setArguments({"status", "--porcelain"});
        proc.start();
        if (!proc.waitForStarted(3000))
            return result;
        if (!proc.waitForFinished(5000))
            return result;

        const QByteArray out = proc.readAllStandardOutput();
        const QList<QByteArray> lines = out.split('\n');
        for (const QByteArray &line : lines)
        {
            if (line.size() < 4)
                continue;
            // 格式：XY filename（X=暂存区状态, Y=工作区状态）
            char x = line.at(0);
            char y = line.at(1);
            QString name = QString::fromUtf8(line.mid(3)).trimmed();
            if (name.size() > 1 && name.startsWith('"') && name.endsWith('"'))
                name = name.mid(1, name.size() - 2);

            int flags = AssetStatus::Clean;
            if (x == 'A' || y == 'A' || x == '?' || y == '?')
                flags |= AssetStatus::Added; // + 新增
            else if (x == 'M' || y == 'M' || x == 'D' || y == 'D')
                flags |= AssetStatus::CheckedOut; // ✓ 已检出/修改

            if (flags != AssetStatus::Clean)
                result[QDir(repo_root_).absoluteFilePath(name)] = flags;
        }
        return result;
    }

    // ─── AssetStore ───────────────────────────────────────────────
    AssetStore::AssetStore(const QString &projectRoot, QObject *parent)
        : QObject(parent), root_(projectRoot), vcs_client_(projectRoot) {}

    void AssetStore::set_project_root(const QString &root)
    {
        root_ = root;
        dirty_.clear();
        vcs_client_ = VcsClient(root);
        vcs_status_.clear();
    }

    void AssetStore::mark_dirty(const QString &path) { dirty_.insert(path); }
    void AssetStore::mark_clean(const QString &path) { dirty_.remove(path); }
    bool AssetStore::is_dirty(const QString &path) const { return dirty_.contains(path); }

    int AssetStore::status_of(const QString &path) const
    {
        int s = AssetStatus::Clean;
        if (dirty_.contains(path))
            s |= AssetStatus::Dirty;
        if (vcs_status_.contains(path))
            s |= vcs_status_.value(path);
        return s;
    }

    void AssetStore::refresh_vcs()
    {
        vcs_status_ = vcs_client_.refresh();
        emit statusesChanged();
    }

    // ─── AssetModel ───────────────────────────────────────────────
    AssetModel::AssetModel(QObject *parent) : QAbstractListModel(parent) {}

    int AssetModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(items_.size());
    }

    QVariant AssetModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
            return {};
        const AssetItem &it = items_[index.row()];
        switch (role)
        {
        case Qt::DisplayRole:
            return it.name;
        case Qt::ToolTipRole:
            return it.path;
        case TypeRole:
            return static_cast<int>(it.type);
        case StatusRole:
            return it.status;
        case PathRole:
            return it.path;
        default:
            return {};
        }
    }

    void AssetModel::set_items(const QVector<AssetItem> &items)
    {
        beginResetModel();
        items_ = items;
        endResetModel();
    }

    const AssetItem &AssetModel::item_at(int row) const { return items_[row]; }

    Qt::ItemFlags AssetModel::flags(const QModelIndex &index) const
    {
        Qt::ItemFlags f = QAbstractListModel::flags(index);
        if (index.isValid())
            f |= Qt::ItemIsDragEnabled;
        return f;
    }

    QStringList AssetModel::mimeTypes() const
    {
        return {"text/uri-list", "application/x-rvdlrsp-asset"};
    }

    QMimeData *AssetModel::mimeData(const QModelIndexList &indexes) const
    {
        QList<QUrl> urls;
        for (const QModelIndex &idx : indexes)
        {
            if (!idx.isValid())
                continue;
            QString path = items_[idx.row()].path;
            urls << QUrl::fromLocalFile(path);
        }
        auto *mime = new QMimeData();
        mime->setUrls(urls);
        return mime;
    }

    // ─── ThumbnailRenderer ────────────────────
    QImage ThumbnailRenderer::render(const QString &path, AssetType type)
    {
        switch (type)
        {
        case AssetType::Texture:
            return render_texture(path);
        case AssetType::Mesh:
            return render_mesh(path);
        default:
            return render_type_icon(type);
        }
    }

    QImage ThumbnailRenderer::render_mesh(const QString &path)
    {
        try
        {
            render::Mesh mesh = render::MeshLoader::load(path.toStdString());
            QImage img = rasterize_mesh(mesh, ThumbSize);
            if (!img.isNull())
                return img;
        }
        catch (const std::exception &)
        {
            // 加载失败 → 回退类型图标
        }
        return render_type_icon(AssetType::Mesh);
    }

    QImage ThumbnailRenderer::render_texture(const QString &path)
    {
        QImage img(path);
        if (img.isNull())
            return render_type_icon(AssetType::Texture);
        return img;
    }

    QImage ThumbnailRenderer::render_type_icon(AssetType type)
    {
        QImage img(ThumbSize, ThumbSize, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);

        QColor bg;
        QString glyph;
        switch (type)
        {
        case AssetType::Robot:
            bg = QColor("#5b4f8b");
            glyph = "R";
            break;
        case AssetType::Mesh:
            bg = QColor("#3f6f8f");
            glyph = "M";
            break;
        case AssetType::Material:
            bg = QColor("#8f5f3f");
            glyph = "A";
            break;
        case AssetType::Scene:
            bg = QColor("#4f8f5f");
            glyph = "S";
            break;
        case AssetType::Script:
            bg = QColor("#4a6b8a");
            glyph = "K";
            break;
        case AssetType::Blueprint:
            bg = QColor("#3f6fa8");
            glyph = "B";
            break;
        case AssetType::Audio:
            bg = QColor("#6f4f8f");
            glyph = "♪";
            break;
        case AssetType::Texture:
            bg = QColor("#8f3f5f");
            glyph = "T";
            break;
        default:
            bg = QColor("#5a5f66");
            glyph = "?";
            break;
        }

        p.setBrush(bg);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRect(4, 4, ThumbSize - 8, ThumbSize - 8), 10, 10);
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(48);
        p.setFont(f);
        p.drawText(QRect(0, 0, ThumbSize, ThumbSize), Qt::AlignCenter, glyph);
        return img;
    }

    // 软件光栅化：透视投影 + z-buffer + Lambert 光照
    QImage ThumbnailRenderer::rasterize_mesh(const render::Mesh &mesh, int size)
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
            return {};

        qpc::Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
        for (const auto &v : mesh.vertices)
        {
            lo.x = std::min(lo.x, v.position.x);
            lo.y = std::min(lo.y, v.position.y);
            lo.z = std::min(lo.z, v.position.z);
            hi.x = std::max(hi.x, v.position.x);
            hi.y = std::max(hi.y, v.position.y);
            hi.z = std::max(hi.z, v.position.z);
        }
        qpc::Vec3 center = (lo + hi) * 0.5;
        double radius = std::max({(hi - lo).x, (hi - lo).y, (hi - lo).z}) * 0.5;
        if (radius < 1e-9)
            radius = 1.0;

        qpc::Vec3 eye(0.0, 0.55, 2.6);
        render::Mat4 proj = render::Mat4::perspective(40.0, 1.0, 0.1, 100.0);
        render::Mat4 view = render::Mat4::look_at(eye, {0, 0, 0}, {0, 1, 0});
        render::Mat4 vp = proj * view;

        const size_t n = mesh.vertices.size();
        std::vector<qpc::Vec3> pts(n);
        std::vector<qpc::Vec3> norms(n);
        for (size_t i = 0; i < n; ++i)
        {
            qpc::Vec3 p = (mesh.vertices[i].position - center) * (1.0 / radius);
            qpc::Vec3 norm = mesh.vertices[i].normal.normalized();
            double cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
            double cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
            double cz = vp.m[2] * p.x + vp.m[6] * p.y + vp.m[10] * p.z + vp.m[14];
            double cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
            if (std::fabs(cw) < 1e-9)
                cw = 1e-9;
            pts[i] = {cx / cw, cy / cw, cz / cw};
            norms[i] = norm;
        }

        QImage img(size, size, QImage::Format_ARGB32);
        img.fill(QColor(28, 32, 38));
        std::vector<float> zbuf(static_cast<size_t>(size) * size, 1e30f);

        qpc::Vec3 light = qpc::Vec3(-0.4, 0.7, 0.6).normalized();

        const size_t tri_count = mesh.indices.size() / 3;
        for (size_t t = 0; t < tri_count; ++t)
        {
            uint32_t ia = mesh.indices[t * 3 + 0];
            uint32_t ib = mesh.indices[t * 3 + 1];
            uint32_t ic = mesh.indices[t * 3 + 2];
            if (ia >= n || ib >= n || ic >= n)
                continue;

            qpc::Vec3 a = pts[ia], b = pts[ib], c = pts[ic];
            double ax = (a.x * 0.5 + 0.5) * size;
            double ay = (0.5 - a.y * 0.5) * size;
            double bx = (b.x * 0.5 + 0.5) * size;
            double by = (0.5 - b.y * 0.5) * size;
            double cx = (c.x * 0.5 + 0.5) * size;
            double cy = (0.5 - c.y * 0.5) * size;

            int minx = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
            int maxx = std::min(size - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
            int miny = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
            int maxy = std::min(size - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));

            double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (std::fabs(area) < 1e-9)
                continue;

            qpc::Vec3 face_n = (b - a).cross(c - a);
            if (face_n.z > 0.0)
                continue;

            qpc::Vec3 n_avg = (norms[ia] + norms[ib] + norms[ic]) * (1.0 / 3.0);
            double ndl = std::max(0.0, n_avg.dot(light));
            double shade = 0.35 + 0.65 * ndl;

            auto shade_color = [&](const render::Vertex &v)
            {
                int r = static_cast<int>(std::min(255.0, (v.r * 255.0 * shade)));
                int g = static_cast<int>(std::min(255.0, (v.g * 255.0 * shade)));
                int b = static_cast<int>(std::min(255.0, (v.b * 255.0 * shade)));
                return qRgb(r, g, b);
            };
            QRgb ca = shade_color(mesh.vertices[ia]);
            QRgb cb = shade_color(mesh.vertices[ib]);
            QRgb cc = shade_color(mesh.vertices[ic]);

            for (int py = miny; py <= maxy; ++py)
            {
                for (int px = minx; px <= maxx; ++px)
                {
                    double sx = px + 0.5, sy = py + 0.5;
                    double w0 = ((bx - ax) * (sy - ay) - (by - ay) * (sx - ax)) / area;
                    double w1 = ((cx - bx) * (sy - by) - (cy - by) * (sx - bx)) / area;
                    double w2 = 1.0 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;

                    double depth = a.z * w0 + b.z * w1 + c.z * w2;
                    size_t zi = static_cast<size_t>(py) * size + px;
                    if (depth >= zbuf[zi])
                        continue;
                    zbuf[zi] = static_cast<float>(depth);

                    int r = static_cast<int>(qRed(ca) * w0 + qRed(cb) * w1 + qRed(cc) * w2);
                    int g = static_cast<int>(qGreen(ca) * w0 + qGreen(cb) * w1 + qGreen(cc) * w2);
                    int b = static_cast<int>(qBlue(ca) * w0 + qBlue(cb) * w1 + qBlue(cc) * w2);
                    img.setPixel(px, py, qRgb(r, g, b));
                }
            }
        }
        return img;
    }

    // ─── AsyncThumbnailLoader ─────────────────────────────────────
    AsyncThumbnailLoader::AsyncThumbnailLoader(QObject *parent) : QObject(parent)
    {
        worker_ = std::thread(&AsyncThumbnailLoader::workerLoop, this);
    }

    AsyncThumbnailLoader::~AsyncThumbnailLoader()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    QImage AsyncThumbnailLoader::thumbnail(const QString &path, AssetType type)
    {
        bool submitted = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = cache_.find(path);
            if (it != cache_.end())
                return it.value(); // 锁随 lock_guard 析构释放

            if (!pending_.contains(path) && pending_.size() < kMaxInFlight)
            {
                pending_.insert(path);
                ++total_submitted_;
                queue_.push_back({path, type});
                submitted = true;
            }
        }
        if (submitted)
        {
            cv_.notify_one();
            emit progressChanged();
        }
        // 未就绪：返回占位类型图标
        return ThumbnailRenderer::render_type_icon(type);
    }

    void AsyncThumbnailLoader::clear_cache()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cache_.clear();
        pending_.clear();
    }

    void AsyncThumbnailLoader::cancel_all()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.clear();
            pending_.clear();
            total_submitted_ = 0;
            total_completed_ = 0;
        }
        cv_.notify_all();
        emit progressChanged();
    }

    void AsyncThumbnailLoader::set_visible(const QSet<QString> &paths)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            visible_ = paths;

            // 取消队列中已滚出视口的任务
            std::deque<Request> kept;
            QSet<QString> cancelled;
            for (const Request &r : queue_)
            {
                if (visible_.contains(r.path))
                    kept.push_back(r);
                else
                    cancelled.insert(r.path);
            }
            queue_.swap(kept);

            // 从 pending 移除被取消的（主线程集合，此处仍在主线程）
            for (const QString &p : cancelled)
                pending_.remove(p);
        }
        cv_.notify_all();
        emit progressChanged();
    }

    void AsyncThumbnailLoader::workerLoop()
    {
        for (;;)
        {
            Request req;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]
                         { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    break;

                // 优先级：优先取"可视区"内的任务，其次按入队顺序取队首
                auto it = std::find_if(queue_.begin(), queue_.end(),
                                       [this](const Request &r)
                                       { return visible_.contains(r.path); });
                if (it == queue_.end())
                    it = queue_.begin();
                req = *it;
                queue_.erase(it);
            }

            QImage img = ThumbnailRenderer::render(req.path, req.type);
            if (img.isNull())
                img = ThumbnailRenderer::render_type_icon(req.type);
            if (img.width() != ThumbnailRenderer::ThumbSize || img.height() != ThumbnailRenderer::ThumbSize)
                img = img.scaled(ThumbnailRenderer::ThumbSize, ThumbnailRenderer::ThumbSize,
                                 Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

            {
                std::lock_guard<std::mutex> lk(mutex_);
                results_.push_back({req.path, img});
            }
            // 回主线程派发结果
            QMetaObject::invokeMethod(this, [this]
                                      { drainResults(); }, Qt::QueuedConnection);
        }
    }

    void AsyncThumbnailLoader::drainResults()
    {
        std::deque<std::pair<QString, QImage>> ready;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ready.swap(results_);
            for (auto &kv : ready)
                visible_.remove(kv.first); // 已完成，移出可视优先级集合
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto &kv : ready)
            {
                cache_[kv.first] = kv.second;
                pending_.remove(kv.first);
            }
            if (!ready.empty())
                total_completed_ += static_cast<int>(ready.size());
        }
        for (auto &kv : ready)
            emit thumbnailReady(kv.first);
        if (!ready.empty())
            emit progressChanged();
    }

    // ─── ThumbnailDelegate ────────────────────────────────────────
    ThumbnailDelegate::ThumbnailDelegate(AsyncThumbnailLoader *loader, QObject *parent)
        : QStyledItemDelegate(parent), loader_(loader) {}

    void ThumbnailDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
    {
        QStyledItemDelegate::paint(painter, option, QModelIndex());

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const AssetType type = static_cast<AssetType>(index.data(AssetModel::TypeRole).toInt());
        const QString path = index.data(AssetModel::PathRole).toString();
        const int status = index.data(AssetModel::StatusRole).toInt();
        const bool dark = ThemeManager::instance().isDark();

        QRect r = option.rect.adjusted(6, 6, -6, -22);

        if (loader_)
        {
            QImage img = loader_->thumbnail(path, type);
            if (!img.isNull())
            {
                QImage scaled = img.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QRect thumb = scaled.rect();
                thumb.moveCenter(r.center());
                painter->drawImage(thumb, scaled);
            }
        }

        if (option.state & QStyle::State_Selected)
        {
            painter->setBrush(Qt::NoBrush);
            QPen pen(dark ? QColor("#3aa0ff") : QColor("#0b57d0"), 2);
            painter->setPen(pen);
            painter->drawRoundedRect(r, 6, 6);
        }

        draw_badge(painter, r, status);

        QRect text_rect(option.rect.left() + 4, option.rect.bottom() - 18,
                        option.rect.width() - 8, 16);
        QString name = index.data(Qt::DisplayRole).toString();
        QFont f = painter->font();
        f.setPixelSize(11);
        painter->setFont(f);
        painter->setPen(dark ? QColor("#c8ccd4") : QColor("#1f1f1f"));
        painter->drawText(text_rect, Qt::AlignHCenter | Qt::AlignVCenter,
                          QFontMetrics(f).elidedText(name, Qt::ElideMiddle, text_rect.width()));

        painter->restore();
    }

    QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const
    {
        (void)option;
        (void)index;
        return QSize(ThumbnailRenderer::ThumbSize + 12, ThumbnailRenderer::ThumbSize + 34);
    }

    void ThumbnailDelegate::draw_badge(QPainter *p, const QRect &thumb_rect, int status) const
    {
        struct Badge
        {
            const char *glyph;
            QColor color;
            int flag;
        };
        const Badge badges[] = {
            {"*", QColor("#f0c040"), AssetStatus::Dirty},
            {"✓", QColor("#4caf50"), AssetStatus::CheckedOut},
            {"+", QColor("#3aa0ff"), AssetStatus::Added},
        };
        int shown = 0;
        for (const auto &b : badges)
        {
            if (!(status & b.flag))
                continue;
            QRect box(thumb_rect.right() - 18 - shown * 20, thumb_rect.top() + 4, 18, 18);
            p->setBrush(b.color);
            p->setPen(QPen(QColor(20, 22, 26), 1));
            p->drawRoundedRect(box, 4, 4);
            QFont f = p->font();
            f.setBold(true);
            f.setPixelSize(12);
            p->setFont(f);
            p->setPen(Qt::white);
            p->drawText(box, Qt::AlignCenter, QString::fromUtf8(b.glyph));
            ++shown;
        }
    }

    // ─── ContentBrowser ───────────────────────────────────────────
    ContentBrowser::ContentBrowser(const QString &projectRoot, QWidget *parent)
        : QWidget(parent), root_(projectRoot)
    {
        build_ui();
        set_project_root(projectRoot);
    }

    void ContentBrowser::build_ui()
    {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);

        auto *top = new QHBoxLayout();
        back_btn_ = new QPushButton("←");
        back_btn_->setFixedWidth(30);
        path_label_ = new QLabel();
        progress_bar_ = new QProgressBar();
        progress_bar_->setRange(0, 100);
        progress_bar_->setValue(0);
        progress_bar_->setTextVisible(true);
        progress_bar_->setFixedWidth(160);
        progress_bar_->setFixedHeight(18);
        progress_bar_->setVisible(false);
        cancel_btn_ = new QPushButton("✕");
        cancel_btn_->setFixedWidth(24);
        cancel_btn_->setToolTip(RVDLTR("取消缩略图渲染"));
        cancel_btn_->setVisible(false);
        filter_ = new QLineEdit();
        filter_->setPlaceholderText(RVDLTR("过滤资产…"));
        filter_->setFixedWidth(180);
        top->addWidget(back_btn_);
        top->addWidget(path_label_, 1);
        top->addWidget(progress_bar_);
        top->addWidget(cancel_btn_);
        top->addWidget(filter_);
        lay->addLayout(top);

        auto *body = new QHBoxLayout();

        fs_model_ = new QFileSystemModel(this);
        fs_model_->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);

        tree_ = new QTreeView();
        tree_->setModel(fs_model_);
        tree_->setHeaderHidden(true);
        tree_->setFixedWidth(240);
        for (int i = 1; i < fs_model_->columnCount(); ++i)
            tree_->hideColumn(i);
        body->addWidget(tree_);

        model_ = new AssetModel(this);
        loader_ = new AsyncThumbnailLoader(this);
        store_ = new AssetStore(root_, this);

        list_ = new QListView();
        list_->setModel(model_);
        list_->setViewMode(QListView::IconMode);
        list_->setResizeMode(QListView::Adjust);
        list_->setMovement(QListView::Static);
        list_->setSpacing(4);
        list_->setIconSize(QSize(ThumbnailRenderer::ThumbSize, ThumbnailRenderer::ThumbSize));
        list_->setItemDelegate(new ThumbnailDelegate(loader_, this));
        list_->setContextMenuPolicy(Qt::CustomContextMenu);
        list_->setDragEnabled(true); // 支持拖拽资产
        list_->setDragDropMode(QAbstractItemView::DragOnly);
        list_->setDefaultDropAction(Qt::CopyAction);

        // ── 详细信息表格视图 ────────────────────────────────────
        detail_model_ = new QFileSystemModel(this);
        detail_model_->setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        detail_view_ = new QTreeView();
        detail_view_->setModel(detail_model_);
        detail_view_->setRootIsDecorated(true);
        detail_view_->setSortingEnabled(true);
        detail_view_->sortByColumn(0, Qt::AscendingOrder);
        detail_view_->setContextMenuPolicy(Qt::CustomContextMenu);
        detail_view_->setColumnWidth(0, 260);
        detail_view_->setColumnWidth(1, 120);
        detail_view_->setColumnWidth(2, 100);

        stack_ = new QStackedWidget();
        stack_->addWidget(list_);        // 索引 0：图标/列表视图
        stack_->addWidget(detail_view_); // 索引 1：详细信息表格
        body->addWidget(stack_, 1);
        lay->addLayout(body, 1);

        connect(cancel_btn_, &QPushButton::clicked, this, [this]()
                {
        loader_->cancel_all();
        update_progress(); });
        connect(list_, &QListView::customContextMenuRequested, this,
                &ContentBrowser::show_context_menu);
        connect(back_btn_, &QPushButton::clicked, this, [this]()
                {
        QDir up(current_dir_);
        up.cdUp();
        navigate_to(up.absolutePath()); });
        connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &idx)
                { navigate_to(fs_model_->filePath(idx)); });
        connect(list_, &QListView::doubleClicked, this, [this](const QModelIndex &idx)
                {
        if (!idx.isValid())
            return;
        const AssetItem &it = model_->item_at(idx.row());
        if (it.type == AssetType::Folder) {
            navigate_to(it.path);
        } else {
            emit assetSelected(it.path);   // 双击选中 → 显示属性
        } });
        connect(list_, &QListView::clicked, this, [this](const QModelIndex &idx)
                {
                    if (!idx.isValid())
                        return;
                    const AssetItem &it = model_->item_at(idx.row());
                    if (it.type != AssetType::Folder)
                        emit assetSelected(it.path); // 单击选中 → 显示属性
                });
        connect(detail_view_, &QTreeView::doubleClicked, this, [this](const QModelIndex &idx)
                {
        if (!idx.isValid())
            return;
        QString path = detail_model_->filePath(idx);
        QFileInfo fi(path);
        if (fi.isDir()) {
            navigate_to(path);
        } else {
            emit assetSelected(path);   // 双击选中 → 显示属性
        } });
        connect(detail_view_, &QTreeView::clicked, this, [this](const QModelIndex &idx)
                {
                    if (!idx.isValid())
                        return;
                    QString path = detail_model_->filePath(idx);
                    if (!QFileInfo(path).isDir())
                        emit assetSelected(path); // 单击选中 → 显示属性
                });
        connect(detail_view_, &QTreeView::customContextMenuRequested, this,
                [this](const QPoint &pos)
                {
                    show_context_menu(pos);
                });
        connect(store_, &AssetStore::statusesChanged, this, [this]()
                { refresh_current_dir(); });
        // 缩略图就绪 → 重绘网格
        connect(loader_, &AsyncThumbnailLoader::thumbnailReady, list_,
                QOverload<>::of(&QWidget::update));
        // 缩略图进度 → 刷新进度标签
        connect(loader_, &AsyncThumbnailLoader::progressChanged, this,
                &ContentBrowser::update_progress);

        // 滚动 / 尺寸变化 → 重算可视集，取消已滚出视口的任务
        connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int)
                { update_visible_set(); });
        connect(list_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int)
                { update_visible_set(); });

        auto *vcs_timer = new QTimer(this);
        vcs_timer->setInterval(3000);
        connect(vcs_timer, &QTimer::timeout, this, [this]()
                { store_->refresh_vcs(); });
        vcs_timer->start();
    }

    void ContentBrowser::set_project_root(const QString &root)
    {
        root_ = root;
        QDir().mkpath(root_);
        store_->set_project_root(root_);
        loader_->clear_cache();
        fs_model_->setRootPath(root_);
        tree_->setRootIndex(fs_model_->index(root_));
        navigate_to(root_);
        store_->refresh_vcs();
    }

    void ContentBrowser::set_dirty(const QString &path, bool dirty)
    {
        if (dirty)
            store_->mark_dirty(path);
        else
            store_->mark_clean(path);
        refresh_current_dir();
    }

    void ContentBrowser::navigate_to(const QString &dir)
    {
        if (dir.isEmpty())
            return;
        current_dir_ = QDir(dir).absolutePath();
        QDir root(root_);
        QDir cur(current_dir_);
        QString rel = root.relativeFilePath(cur.absolutePath());
        if (rel.startsWith(".."))
        {
            current_dir_ = root_;
            cur = QDir(root_);
        }
        path_label_->setText(cur.absolutePath());
        tree_->setCurrentIndex(fs_model_->index(cur.absolutePath()));
        if (detail_model_)
        {
            detail_model_->setRootPath(cur.absolutePath());
            detail_view_->setRootIndex(detail_model_->index(cur.absolutePath()));
        }
        refresh_current_dir();
    }

    void ContentBrowser::refresh_current_dir()
    {
        QVector<AssetItem> items;
        QDir dir(current_dir_);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);

        for (const QFileInfo &fi : entries)
        {
            AssetItem it;
            it.name = fi.fileName();
            it.path = fi.absoluteFilePath();
            it.type = asset_type_from_path(it.path);
            it.status = store_->status_of(it.path);
            items.push_back(it);
        }
        model_->set_items(items);
        update_visible_set();
    }

    void ContentBrowser::update_progress()
    {
        if (!loader_ || !progress_bar_)
            return;
        const int total = loader_->total_submitted();
        const int done = loader_->total_completed();

        if (total > 0 && done < total)
        {
            const int pct = done * 100 / total;
            progress_bar_->setValue(pct);
            progress_bar_->setFormat(RVDLTR("缩略图 %1 / %2 (%3%)").arg(done).arg(total).arg(pct));
            progress_bar_->setVisible(true);
            if (cancel_btn_)
                cancel_btn_->setVisible(true);
        }
        else
        {
            // 一批完成，隐藏进度条和取消按钮
            progress_bar_->setVisible(false);
            if (cancel_btn_)
                cancel_btn_->setVisible(false);
        }
    }

    void ContentBrowser::update_visible_set()
    {
        if (!list_ || !loader_ || !model_)
            return;

        QSet<QString> visible;
        const QRect viewport = list_->viewport()->rect();

        for (int i = 0; i < model_->rowCount(); ++i)
        {
            QModelIndex idx = model_->index(i, 0);
            QRect item_rect = list_->visualRect(idx);
            if (item_rect.intersects(viewport))
                visible.insert(idx.data(AssetModel::PathRole).toString());
        }

        loader_->set_visible(visible);
    }

    void ContentBrowser::set_view_mode(ViewMode mode)
    {
        view_mode_ = mode;
        switch (mode)
        {
        case ViewMode::ExtraLargeIcons:
            stack_->setCurrentWidget(list_);
            list_->setViewMode(QListView::IconMode);
            list_->setIconSize(QSize(256, 256));
            break;
        case ViewMode::LargeIcons:
            stack_->setCurrentWidget(list_);
            list_->setViewMode(QListView::IconMode);
            list_->setIconSize(QSize(ThumbnailRenderer::ThumbSize, ThumbnailRenderer::ThumbSize));
            break;
        case ViewMode::List:
            stack_->setCurrentWidget(list_);
            list_->setViewMode(QListView::ListMode);
            break;
        case ViewMode::Details:
            stack_->setCurrentWidget(detail_view_);
            break;
        }
        update_visible_set();
    }

    void ContentBrowser::show_context_menu(const QPoint &pos)
    {
        // 确定右键点击处的资产（区分图标视图 / 详情视图）
        context_path_.clear();
        if (stack_->currentWidget() == list_)
        {
            QModelIndex idx = list_->indexAt(pos);
            if (idx.isValid())
                context_path_ = model_->item_at(idx.row()).path;
        }
        else
        {
            QModelIndex idx = detail_view_->indexAt(pos);
            if (idx.isValid())
                context_path_ = detail_model_->filePath(idx);
        }

        QMenu menu(this);

        // ── 打开（右键点击的是文件时）───────────────
        const bool is_file = !context_path_.isEmpty() && !QFileInfo(context_path_).isDir();
        QAction *open_act = menu.addAction(RVDLTR("打开"), this, [this]()
                                           {
        if (!context_path_.isEmpty())
            emit assetActivated(context_path_); });
        open_act->setEnabled(is_file);

        menu.addSeparator();

        // ── 新建子菜单 ──────────────────────────────
        QMenu *new_menu = menu.addMenu(RVDLTR("新建"));
        new_menu->addAction(RVDLTR("新建文件夹"), this, [this]
                            { create_asset(AssetType::Folder); });
        new_menu->addAction(RVDLTR("Rvdl 脚本 (.rvdl)"), this, [this]
                            { create_asset(AssetType::Script); });
        new_menu->addAction(RVDLTR("场景 (.qscene)"), this, [this]
                            { create_asset(AssetType::Scene); });
        new_menu->addAction(RVDLTR("蓝图 (.qbp)"), this, [this]
                            { create_asset(AssetType::Blueprint); });
        new_menu->addAction(RVDLTR("材质 (.qmat)"), this, [this]
                            { create_asset(AssetType::Material); });
        new_menu->addAction(RVDLTR("机器人 (.qrobot)"), this, [this]
                            { create_asset(AssetType::Robot); });
        new_menu->addAction(RVDLTR("文本 (.txt)"), this, [this]
                            { create_asset(AssetType::Unknown); });

        menu.addAction(RVDLTR("粘贴"), this, &ContentBrowser::paste_from_clipboard);
        menu.addSeparator();

        // ── 查看子菜单 ──────────────────────────────
        QMenu *view_menu = menu.addMenu(RVDLTR("查看"));
        auto add_view = [&](const QString &label, ViewMode m)
        {
            QAction *a = view_menu->addAction(label);
            a->setCheckable(true);
            a->setChecked(view_mode_ == m);
            connect(a, &QAction::triggered, this, [this, m]
                    { set_view_mode(m); });
        };
        add_view(RVDLTR("超大图标"), ViewMode::ExtraLargeIcons);
        add_view(RVDLTR("大图标"), ViewMode::LargeIcons);
        add_view(RVDLTR("列表"), ViewMode::List);
        add_view(RVDLTR("详细信息"), ViewMode::Details);

        menu.addSeparator();
        menu.addAction(RVDLTR("刷新"), this, [this]
                       { refresh_current_dir(); });

        // 图标视图和详情视图的坐标映射
        QPoint global;
        if (stack_->currentWidget() == list_)
            global = list_->viewport()->mapToGlobal(pos);
        else
            global = detail_view_->viewport()->mapToGlobal(pos);
        menu.exec(global);
    }

    void ContentBrowser::create_asset(AssetType type)
    {
        bool ok = false;
        QString name = QInputDialog::getText(this, RVDLTR("新建资产"), RVDLTR("名称："), QLineEdit::Normal, "", &ok);
        if (!ok || name.trimmed().isEmpty())
            return;

        QString ext;
        switch (type)
        {
        case AssetType::Folder:
            break;
        case AssetType::Script:
            ext = ".rvdl";
            break;
        case AssetType::Scene:
            ext = ".qscene";
            break;
        case AssetType::Blueprint:
            ext = ".qbp";
            break;
        case AssetType::Material:
            ext = ".qmat";
            break;
        case AssetType::Robot:
            ext = ".qrobot";
            break;
        default:
            ext = ".txt";
            break;
        }

        QDir dir(current_dir_);
        if (type == AssetType::Folder)
        {
            dir.mkdir(name);
        }
        else
        {
            QFile f(dir.absoluteFilePath(name + ext));
            if (f.open(QIODevice::WriteOnly))
                f.close();
        }
        refresh_current_dir();
    }

    void ContentBrowser::paste_from_clipboard()
    {
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        if (!mime)
            return;
        QDir dir(current_dir_);
        if (mime->hasUrls())
        {
            for (const QUrl &url : mime->urls())
            {
                if (!url.isLocalFile())
                    continue;
                QString src = url.toLocalFile();
                QFileInfo fi(src);
                QString dst = dir.absoluteFilePath(fi.fileName());
                if (src != dst)
                    QFile::copy(src, dst);
            }
        }
        else if (mime->hasText())
        {
            QString name = RVDLTR("粘贴文本.txt");
            QFile f(dir.absoluteFilePath(name));
            if (f.open(QIODevice::WriteOnly))
            {
                f.write(mime->text().toUtf8());
                f.close();
            }
        }
        refresh_current_dir();
    }
}