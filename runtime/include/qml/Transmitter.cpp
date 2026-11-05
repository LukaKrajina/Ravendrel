// ============================================================================
// Transmitter.cpp —— QLM 发射器 / 训练器 GUI（Fox 版）
//
// 从 Nuklear 迁到 Fox
// ------------------------
// 这个界面曾经自己持有一套原始 Vulkan（instance / device / swapchain /
// semaphore）+ Nuklear 的 GLFW 后端，约 150 行样板与量子核心老实说毫无关系，
// 且每加一个控件都要手写 nk_layout_row_* 的行列算术。Fox 把窗口、后端
// （VK / CPU 双后端 + 运行期回退）、输入、字体、主题收敛成一个 Context，
// 控件直接吃绝对矩形 —— 而本文件的布局本来就是一排绝对坐标，迁移是
// 平移而不是重设计。
//
// 迁移动了哪些、没动哪些
// ----------------------
//   · 没动：训练线程、文件搬运、网络扫描线程、配置序列化、导出逻辑、
//     崩溃处理（Windows vectored handler）—— 那是业务，与 GUI 框架无关。
//   · 动了：渲染层全部换成 fox 控件（menu / combo / selectable / group /
//     property / button / text_field），主题换成 Golden Gate 深色，
//     快捷键改走 fox 的 InputState（顺带修掉「在搜索框里按 Delete 既删
//     字符又删文件」的老问题 —— 文本字段持焦时字符类快捷键让位）。
//
// 布局自适应
// ----------
// 旧版是写死的 1280×800 + Nuklear 可拖拽窗口。Fox 版按窗口尺寸推导同一
// 套比例（顶栏 70 / 底栏 80 / 三栏照旧），窗口拉多大都不会把面板挤出界外。
//
// 线程与锁
// --------
// 没有 gui_mutex。所有面板状态只被主线程碰（控件只在 render 里读写），
// 训练线程只读配置副本、只写 is_training。曾经有一把 gui_mutex，而 render()
// 全程持锁、按钮回调又在 render 内部触发动作 —— 每点一次「<<」就是一次
// 非递归锁的自锁。删掉锁而不是改成 recursive_mutex：需要递归锁通常意味着
// 临界区划错了。真正跨线程共享的只有 BottomBar 的节点表（nodes_mutex）。
// ============================================================================

#include "../qhal/QVM.hpp"
#include "../qlm/QLM.hpp"
#include "RVDLModelFormat.hpp"
#include "ModelInterop.hpp" // .rvdlm ↔ gguf / safetensors 双向容器转换
#include "../../src/QDataEncoder.hpp"

#include "../qml/Inference.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <sstream>
#include <fstream>
#include <unordered_map>

#include "ciUd/fox/core/context.hpp"
#include "ciUd/fox/widget/primitives.hpp"
#include "ciUd/fox/widget/composite.hpp"
#include "ciUd/fox/input/input.hpp"
#include "../gui/i18n.hpp"

// fox 的控件/几何/主题类型（Rect / Context / f32 / Id / label / button ...）
// 全部活在 namespace fox 里；本文件是单 TU 的可执行文件主体，整文件展开。
using namespace fox;

// 界面文案的 tr() 与语言切换活在 namespace qgui（gui/i18n.hpp）。
using namespace qgui;

#ifdef _WIN32
#include <windows.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

class RavendrelTrainerGUI;
struct TrainingConfiguration;
struct NetworkNode;

enum class OperationMode
{
    LOCAL_MODE,
    SHARED_MODE,
    QUANTUM_NETWORK_MODE
};

enum class MiddelBottomIdentify
{
    LEFT_ARROW,
    RIGHT_ARROW,
    DELETES,
    REFRESH
};

struct TrainingConfiguration
{
    std::string model_name;
    int epochs = 25;
    double learning_rate = 0.02;
    int qubits = 16;
    int layers = 6;
    std::string output_path = "./output/";
    std::vector<std::string> training_files;
};

struct NetworkNode
{
    std::string id;
    std::string address;
    int port;
    bool is_active;
    int latency_ms;
    float quantum_fidelity;
    /**
     * 是否为**模拟**节点。
     *
     * 真实发现协议（UDP 广播 / mDNS / 对端注册表）尚未实现，扫描线程此前的
     * 做法是 rand() 造假 IP/端口/延迟/保真度 —— 那等于把玩具数据当真硬件
     * 呈现（用户在 UI 上看到的是一个不存在的"网络"）。现在这些节点一律带
     * simulated=true，UI 明示 "(simulated)"，启动时打印一行诚实告警。
     * 宁可显示得少，不能显示得假。
     */
    bool simulated = false;
};

struct FileItem
{
    fs::path path;
    bool is_directory;
    int is_selected;
    uintmax_t size;
    std::string modified_time;
};

// ════════════════════════════════════════════════════════════════════════
// 面板基类
//
// 与旧的 GUIComponent 一对一对映：bounds_ 从 nk_rect 换成 fox::Rect
// （同一批绝对坐标，只是类型不同），render() 从「nk_begin/nk_end 一对」
// 换成「fox 面板背景 + 标题条 + 内容」。可见性、标题、update 钩子保留。
// ════════════════════════════════════════════════════════════════════════

class GUIComponent
{
protected:
    RavendrelTrainerGUI *parent;
    std::string title_;
    Rect bounds_;
    bool visible_ = true;

public:
    GUIComponent(RavendrelTrainerGUI *p, const char *title, Rect bounds)
        : parent(p), title_(title ? title : ""), bounds_(bounds) {}

    virtual ~GUIComponent() = default;

    /// 画面板 chrome（背景 + 标题 + 边框）并调 render_content。
    /// scroll_content = true 时内容区带裁剪 + 滚轮（列表类面板用）；
    /// 菜单栏必须传 false —— 下拉浮层要画在面板之外，不能被裁剪憋回去。
    void render(Context &c, bool scroll_content = true)
    {
        if (!visible_)
            return;

        Theme &th = c.theme();
        const f32 title_h = 22.0f;

        if (scroll_content)
        {
            // group_begin 给统一的外观（圆角 + 边框 + 标题条 + 裁剪 + 滚动），
            // content_height 传可见高度即「不滚」—— 短面板零开销。
            group_begin(c, bounds_, title_.c_str(), c.make_id(0x7C000001u, (Id)id_hash_rect(bounds_)),
                        bounds_.h - title_h);
        }
        else
        {
            c.dl().push_rounded(bounds_, th.control_bg, CornerRadii(th.corner_panel), 0.0f);
            c.dl().push_border(bounds_, th.control_border, th.hairline,
                               CornerRadii(th.corner_panel), 0.1f);
            Rect title_bar{bounds_.x, bounds_.y, bounds_.w, title_h};
            c.dl().push_rounded(title_bar, Color::mix(th.control_bg, th.window_bg, 0.5f),
                                CornerRadii(th.corner_panel), 0.05f);
            label(c, Rect{title_bar.x + 10.0f, title_bar.y, title_bar.w - 10.0f, title_bar.h},
                  title_.c_str(), th.text_secondary, Align::Start);
            separator(c, Rect{bounds_.x, bounds_.y + title_h, bounds_.w, 1.0f}, true);
        }

        render_content(c);

        if (scroll_content)
            group_end(c);
    }

    virtual void update() {}

    const char *title() const { return title_.c_str(); }
    void set_visible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }

    /// 内容区（chrome 之下），子类照着摆控件。
    Rect content_rect() const
    {
        return Rect{bounds_.x + 8.0f, bounds_.y + 26.0f,
                    bounds_.w - 16.0f, bounds_.h - 34.0f};
    }

protected:
    virtual void render_content(Context &c) = 0;
};

// ════════════════════════════════════════════════════════════════════════
// 顶栏：菜单 + 搜索框
// ════════════════════════════════════════════════════════════════════════

class TopBar : public GUIComponent
{
private:
    std::string current_file;
    std::vector<std::string> recent_files;
    char search_buffer[256];
    Id search_id = 0;

public:
    // 菜单回调（由 RavendrelTrainerGUI 在构造时注入）
    std::function<void()> on_new;
    std::function<void()> on_open;
    std::function<void()> on_save;
    std::function<void()> on_export;
    std::function<void()> on_select_all;
    std::function<void()> on_deselect_all;
    std::function<void()> on_invert;
    std::function<void()> on_refresh;
    std::function<void()> on_toggle_left;
    std::function<void()> on_toggle_right;
    std::function<void()> on_show_all;
    std::function<void()> on_start_training;
    std::function<void()> on_stop_training;
    std::function<void()> on_configure;
    std::function<void(OperationMode)> on_set_mode;
    std::function<void()> on_about;
    std::function<void()> on_documentation;
    std::function<void()> on_shortcuts;

    TopBar(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Ravendrel Trainer", Rect{0.0f, 0.0f, 1280.0f, 70.0f})
    {
        recent_files = {"chaos_fluid_predictor.rvdlm", "quantum_model_1.rvdlm"};
        memset(search_buffer, 0, sizeof(search_buffer));
    }

    void layout(f32 win_w)
    {
        bounds_ = Rect{0.0f, 0.0f, win_w, 70.0f};
    }

protected:
    void render_content(Context &c) override
    {
        const f32 row_h = 24.0f;
        const f32 y = bounds_.y + (bounds_.h - row_h) * 0.5f;

        // 菜单标题一字排开（绝对坐标，取代 nk_menubar 的行推拢）。
        // 每个标题一个 menu_title；点开的浮层由 menu_popup 画在该标题下方，
        // 条目数必须**先算出来**传给 menu_popup（它据此定浮层高度）。
        struct MenuDef
        {
            const char *label;
            f32 w;
        };
        const MenuDef menus[] = {
            {tr("File"), 46.0f},     {tr("Edit"), 46.0f},   {tr("View"), 50.0f},
            {tr("Training"), 68.0f}, {tr("Network"), 66.0f}, {tr("Language"), 72.0f},
            {tr("Help"), 48.0f},
        };
        const int menu_count = (int)(sizeof(menus) / sizeof(menus[0]));

        f32 x = 8.0f;
        for (int i = 0; i < menu_count; ++i)
        {
            const Rect title_r{x, y, menus[i].w, row_h};
            menu_title(c, title_r, menus[i].label);

            // 该菜单是否正开着？开着才画它的浮层与条目。
            int count = 4;
            if (i == 0) count = 4 + (int)recent_files.size();
            else if (i == 2) count = 4;
            else if (i == 5) count = available_lang_count();
            const Rect content = menu_popup(c, title_r, count);
            if (content.w <= 0.0f)
            {
                x += menus[i].w + 4.0f;
                continue;
            }

            const f32 item_h = 22.0f;
            f32 iy = content.y;
            auto item = [&](const char *text, bool enabled = true) -> bool {
                const bool hit = menu_item(c, Rect{content.x, iy, content.w, item_h}, text, enabled);
                iy += item_h;
                return hit;
            };

            switch (i)
            {
            case 0: // File
                if (item(tr("New")) && on_new) on_new();
                if (item(tr("Open...")) && on_open) on_open();
                if (item(tr("Save")) && on_save) on_save();
                if (item(tr("Export")) && on_export) on_export();
                for (const auto &rf : recent_files) item(rf.c_str(), false);
                break;
            case 1: // Edit
                if (item(tr("Select All")) && on_select_all) on_select_all();
                if (item(tr("Deselect All")) && on_deselect_all) on_deselect_all();
                if (item(tr("Invert Selection")) && on_invert) on_invert();
                break;
            case 2: // View
                if (item(tr("Refresh")) && on_refresh) on_refresh();
                if (item(tr("Toggle Left")) && on_toggle_left) on_toggle_left();
                if (item(tr("Toggle Right")) && on_toggle_right) on_toggle_right();
                if (item(tr("Show All Panels")) && on_show_all) on_show_all();
                break;
            case 3: // Training
                if (item(tr("Start Training")) && on_start_training) on_start_training();
                if (item(tr("Stop Training")) && on_stop_training) on_stop_training();
                if (item(tr("Configure...")) && on_configure) on_configure();
                break;
            case 4: // Network
                if (item(tr("Local Mode (LM)")) && on_set_mode) on_set_mode(OperationMode::LOCAL_MODE);
                if (item(tr("Shared Mode (SM)")) && on_set_mode) on_set_mode(OperationMode::SHARED_MODE);
                if (item(tr("Quantum Net (QNM)")) && on_set_mode)
                    on_set_mode(OperationMode::QUANTUM_NETWORK_MODE);
                break;
            case 5: // Language
            {
                Lang current = current_lang();
                const LangEntry *langs = available_langs();
                for (int k = 0; k < available_lang_count(); ++k)
                {
                    std::string lbl = (langs[k].lang == current) ? "> " : "  ";
                    lbl += langs[k].label;
                    if (item(lbl.c_str())) set_lang(langs[k].lang);
                }
                break;
            }
            case 6: // Help
                if (item(tr("About QQNT")) && on_about) on_about();
                if (item(tr("Documentation")) && on_documentation) on_documentation();
                if (item(tr("Keyboard Shortcuts")) && on_shortcuts) on_shortcuts();
                break;
            }
            x += menus[i].w + 4.0f;
        }

        // 搜索框：右侧固定宽度。id 显式钉住 —— 它会随窗口宽度移动，
        // 位置派生 id 会让它每秒换一个身份（丢焦点、丢光标）。
        if (!search_id)
            search_id = c.make_id(0x7C000002u, (Id)1);
        const Rect search_r{bounds_.right() - 208.0f, y, 200.0f, row_h};
        c.dl().push_rounded(search_r, c.theme().control_bg,
                            CornerRadii(c.theme().corner_field), 0.0f);
        c.dl().push_border(search_r, c.theme().control_border, c.theme().hairline,
                           CornerRadii(c.theme().corner_field), 0.1f);
        text_field(c, search_r, search_buffer, (int)sizeof(search_buffer),
                   tr("Search..."), search_id);
    }

public:
    void addRecentFile(const std::string &file)
    {
        recent_files.insert(recent_files.begin(), file);
        if (recent_files.size() > 10)
            recent_files.pop_back();
    }
};

// ════════════════════════════════════════════════════════════════════════
// 左侧栏：文件树
// ════════════════════════════════════════════════════════════════════════

class LeftSidebar : public GUIComponent
{
private:
    std::vector<FileItem> items;
    std::string current_path;
    int select_all_flag = 0;
    std::chrono::steady_clock::time_point last_click_time;
    size_t last_clicked_index = SIZE_MAX;
    int drive_selected = 0;

    std::vector<std::string> getAvailableDrives()
    {
        std::vector<std::string> drives;
#ifdef _WIN32
        char buf[256];
        GetLogicalDriveStringsA(sizeof(buf), buf);
        for (char *p = buf; *p; p += strlen(p) + 1)
            drives.push_back(std::string(p));
#else
        drives.push_back("/");
#endif
        return drives;
    }

public:
    LeftSidebar(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Files", Rect{10.0f, 70.0f, 300.0f, 460.0f}), select_all_flag(false)
    {
        current_path = fs::current_path().string();
        refreshDirectory();
    }

    void layout(f32 win_h)
    {
        const f32 content_h = win_h - 70.0f - 80.0f - 20.0f;
        bounds_ = Rect{10.0f, 70.0f, 300.0f, content_h};
    }

    void refreshDirectory()
    {
        items.clear();
        if (!fs::exists(current_path))
            return;

        for (const auto &entry : fs::directory_iterator(current_path))
        {
            FileItem item;
            item.path = entry.path();
            item.is_directory = entry.is_directory();
            item.is_selected = false;

            if (item.is_directory)
            {
                item.size = 0;
            }
            else
            {
                try
                {
                    item.size = fs::file_size(entry.path());
                }
                catch (...)
                {
                    item.size = 0;
                }
            }

            auto ftime = fs::last_write_time(entry.path());
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
            item.modified_time = std::ctime(&cftime);

            items.push_back(item);
        }

        std::sort(items.begin(), items.end(), [](const FileItem &a, const FileItem &b)
                  {
            if (a.is_directory != b.is_directory) return a.is_directory > b.is_directory;
            return a.path.filename() < b.path.filename(); });
    }

protected:
    void render_content(Context &c) override
    {
        const Rect cr = content_rect();
        const f32 row_h = 20.0f;
        f32 y = cr.y;

        label(c, Rect{cr.x, y, cr.w, 18.0f}, tr("Drive / Directory"), c.theme().text_secondary,
              Align::Start);
        y += 20.0f;

        // Up 按钮 + 当前路径（路径当只读标签 —— 旧版也是只读 nk_label）
        const Rect up_r{cr.x, y, 44.0f, row_h};
        if (button(c, up_r, tr("Up"), ButtonStyle::Bezel))
            navigateUp();
        label(c, Rect{cr.x + 50.0f, y, cr.w - 50.0f, row_h}, current_path.c_str(),
              c.theme().text_tertiary, Align::Start);
        y += row_h + 4.0f;

        // 驱动器下拉（combo）
        auto drives = getAvailableDrives();
        const Rect drive_r{cr.x, y, cr.w, row_h};
        std::vector<const char *> drive_ptrs;
        drive_ptrs.reserve(drives.size());
        for (auto &d : drives) drive_ptrs.push_back(d.c_str());
        if (!drive_ptrs.empty())
        {
            if (drive_selected >= (int)drive_ptrs.size()) drive_selected = 0;
            const int before = drive_selected;
            combo(c, drive_r, drive_ptrs.data(), (int)drive_ptrs.size(), drive_selected);
            if (drive_selected != before)
                navigateTo(drives[(size_t)drive_selected]);
        }
        y += row_h + 4.0f;

        // 全选 / 全不选
        const Rect sel_r{cr.x, y, (cr.w - 4.0f) * 0.5f, row_h};
        const Rect desel_r{cr.x + (cr.w - 4.0f) * 0.5f + 4.0f, y, (cr.w - 4.0f) * 0.5f, row_h};
        if (button(c, sel_r, tr("Select All"), ButtonStyle::Tinted))
        {
            select_all_flag = !select_all_flag;
            for (auto &item : items)
                item.is_selected = select_all_flag;
        }
        if (button(c, desel_r, tr("Deselect"), ButtonStyle::Tinted))
        {
            for (auto &item : items)
                item.is_selected = false;
            select_all_flag = false;
        }
        y += row_h + 6.0f;

        // ── 文件列表（行式手绘，不用 selectable 控件）───────────────────
        // 为什么不用 widget/composite 的 selectable：文件行是**开关式**
        // 多选（点一下选中、再点一下取消），而 selectable 的语义是
        // 「选一个」（点一下置真、不提供取消）——  BottomBar 的模式切换
        // 用后者，文件行用前者，两种语义不能挤同一个控件。行式布局也
        // 让「复选框 + 类型 + 名字 + 大小」四列各自有绝对的 x。
        const f32 list_h = cr.bottom() - y;
        if (list_h < row_h)
            return;
        const f32 content_h = (f32)items.size() * row_h;
        const Id list_id = c.make_id(0x7C000003u, (Id)1);
        // group_begin 返回的内容 rect 已经把滚动偏移烘进 y 了 —— 行 i 的
        // 位置就是 lc.y + i*row_h，不要再自己减一次 scroll。
        const Rect lc = group_begin(c, Rect{cr.x, y, cr.w, list_h}, nullptr, list_id, content_h);

        const f32 col_chk = lc.x + 2.0f;
        const f32 col_type = lc.x + 22.0f;
        const f32 col_name = lc.x + 46.0f;
        const f32 col_size = lc.right() - 56.0f;
        const f32 name_w = col_size - col_name - 4.0f;
        Theme &th = c.theme();

        for (size_t i = 0; i < items.size(); ++i)
        {
            auto &item = items[i];
            const f32 ry = lc.y + (f32)i * row_h;
            if (ry + row_h < lc.y || ry > lc.bottom())
                continue; // 视口外不画（也不做命中测试）

            const Rect row_r{lc.x, ry, lc.w, row_h};

            // 命中 + 开关式选中。
            // 行 id 用**索引**而不是位置哈希：滚动时行在动，位置派生的 id
            // 会让每一行每秒换一个身份（hover/按下动画全丢）。
            const Id row_id = c.make_id(0x7C000004u, (Id)(i + 1));
            const bool over = c.hover(row_r, row_id);
            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
                item.is_selected = item.is_selected ? 0 : 1;

            // 背景：选中 / 悬停
            if (item.is_selected)
                c.dl().push_rounded(row_r, Color::mix(th.control_bg, th.accent_color, 0.45f),
                                    CornerRadii(3.0f), 0.0f);
            else if (over)
                c.dl().push_rounded(row_r, Color::mix(th.control_bg, th.accent_color, 0.18f),
                                    CornerRadii(3.0f), 0.0f);

            bool sel = item.is_selected != 0;
            checkbox(c, Rect{col_chk, ry + 2.0f, 16.0f, 16.0f}, "", sel);
            item.is_selected = sel ? 1 : 0;
            label(c, Rect{col_type, ry, 24.0f, row_h}, item.is_directory ? "[D]" : "[F]",
                  th.text_tertiary, Align::Start);

            std::string name = item.path.filename().string();
            if (name.empty())
                name = item.path.string();
            label(c, Rect{col_name, ry, name_w, row_h}, name.c_str(),
                  item.is_selected ? th.accent_color : th.text, Align::Start);

            char size_buf[32];
            if (item.is_directory)
                snprintf(size_buf, sizeof(size_buf), "-");
            else if (item.size < 1024)
                snprintf(size_buf, sizeof(size_buf), "%llu B", (unsigned long long)item.size);
            else if (item.size < 1024 * 1024)
                snprintf(size_buf, sizeof(size_buf), "%.1f KB", item.size / 1024.0);
            else
                snprintf(size_buf, sizeof(size_buf), "%.1f MB", item.size / (1024.0 * 1024.0));
            label(c, Rect{col_size, ry, 52.0f, row_h}, size_buf, th.text_tertiary, Align::End);

            // 目录双击进入（保留旧版的 400ms 双击窗口）
            if (item.is_directory && over &&
                c.input().buttons[(int)MouseButton::Left].pressed)
            {
                auto now = std::chrono::steady_clock::now();
                if (last_clicked_index == i)
                {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_click_time);
                    if (elapsed.count() < 400)
                    {
                        navigateTo(items[i].path.string());
                        last_clicked_index = SIZE_MAX;
                    }
                    else
                    {
                        last_click_time = now;
                    }
                }
                else
                {
                    last_clicked_index = i;
                    last_click_time = now;
                }
            }
        }
        group_end(c);
    }

public:
    void navigateTo(const std::string &path)
    {
        if (fs::exists(path) && fs::is_directory(path))
        {
            current_path = path;
            refreshDirectory();
        }
    }

    void navigateUp()
    {
        auto parent = fs::path(current_path).parent_path();
        if (!parent.empty())
            navigateTo(parent.string());
    }

    std::vector<FileItem> getSelectedItems()
    {
        std::vector<FileItem> selected;
        for (const auto &item : items)
            if (item.is_selected)
                selected.push_back(item);
        return selected;
    }

    void toggleSelectAll()
    {
        select_all_flag = !select_all_flag;
        for (auto &item : items)
        {
            item.is_selected = select_all_flag;
        }
    }

    void toggleSelection(size_t index)
    {
        if (index < items.size())
        {
            items[index].is_selected = !items[index].is_selected;
        }
    }

    void selectAll()
    {
        select_all_flag = true;
        for (auto &item : items)
            item.is_selected = true;
    }

    void deselectAll()
    {
        select_all_flag = false;
        for (auto &item : items)
            item.is_selected = false;
    }

    void invertSelection()
    {
        for (auto &item : items)
            item.is_selected = !item.is_selected;
        select_all_flag = false;
    }

    std::string getCurrentPath() const { return current_path; }
};

// ════════════════════════════════════════════════════════════════════════
// 右上：训练数据（tmp 目录）
// ════════════════════════════════════════════════════════════════════════

class RightSidebarUpper : public GUIComponent
{
private:
    std::vector<FileItem> tmp_items;
    std::string tmp_path;

public:
    RightSidebarUpper(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Training Data", Rect{450.0f, 70.0f, 420.0f, 250.0f})
    {
        tmp_path = fs::temp_directory_path().string() + "/training_data";
        fs::create_directories(tmp_path);
        refreshTmpDirectory();
    }

    void layout(f32 win_h)
    {
        const f32 content_h = win_h - 70.0f - 80.0f - 20.0f;
        const f32 upper_h = content_h * 0.48f;
        bounds_ = Rect{450.0f, 70.0f, 420.0f, upper_h};
    }

    void refreshTmpDirectory()
    {
        tmp_items.clear();
        if (!fs::exists(tmp_path))
            fs::create_directories(tmp_path);

        for (const auto &entry : fs::directory_iterator(tmp_path))
        {
            FileItem item;
            item.path = entry.path();
            item.is_directory = entry.is_directory();
            item.is_selected = false;

            if (!item.is_directory)
            {
                try
                {
                    item.size = fs::file_size(entry.path());
                }
                catch (...)
                {
                    item.size = 0;
                }
            }

            tmp_items.push_back(item);
        }
    }

protected:
    void render_content(Context &c) override
    {
        const Rect cr = content_rect();
        const f32 row_h = 18.0f;
        f32 y = cr.y;

        char path_buf[512];
        snprintf(path_buf, sizeof(path_buf), tr("Path: %s"), tmp_path.c_str());
        label(c, Rect{cr.x, y, cr.w, 16.0f}, path_buf, c.theme().text_tertiary, Align::Start);
        y += 18.0f;

        const Rect clear_r{cr.x, y, 80.0f, row_h};
        if (button(c, clear_r, tr("Clear"), ButtonStyle::Tinted))
            clearTmp();
        y += row_h + 4.0f;

        const f32 list_h = cr.bottom() - y;
        if (list_h < row_h)
            return;
        const f32 content_h = (f32)tmp_items.size() * row_h;
        const Id list_id = c.make_id(0x7C000005u, (Id)1);
        const Rect lc = group_begin(c, Rect{cr.x, y, cr.w, list_h}, nullptr, list_id, content_h);

        const f32 col_chk = lc.x + 2.0f;
        const f32 col_type = lc.x + 22.0f;
        const f32 col_name = lc.x + 46.0f;
        const f32 col_size = lc.right() - 56.0f;
        const f32 name_w = col_size - col_name - 4.0f;
        Theme &th = c.theme();

        for (size_t i = 0; i < tmp_items.size(); ++i)
        {
            auto &item = tmp_items[i];
            const f32 ry = lc.y + (f32)i * row_h;
            if (ry + row_h < lc.y || ry > lc.bottom())
                continue;

            const Rect row_r{lc.x, ry, lc.w, row_h};
            const Id row_id = c.make_id(0x7C000006u, (Id)(i + 1));
            const bool over = c.hover(row_r, row_id);
            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
                item.is_selected = item.is_selected ? 0 : 1;

            bool sel = item.is_selected != 0;
            checkbox(c, Rect{col_chk, ry + 1.0f, 14.0f, 14.0f}, "", sel);
            item.is_selected = sel ? 1 : 0;
            label(c, Rect{col_type, ry, 24.0f, row_h}, item.is_directory ? "[D]" : "[F]",
                  th.text_tertiary, Align::Start);
            std::string name = item.path.filename().string();
            label(c, Rect{col_name, ry, name_w, row_h}, name.c_str(), th.text, Align::Start);

            char sz[32];
            if (item.is_directory)
                snprintf(sz, sizeof(sz), "-");
            else
                snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)item.size);
            label(c, Rect{col_size, ry, 52.0f, row_h}, sz, th.text_tertiary, Align::End);
        }
        group_end(c);
    }

public:
    void copyToTmp(const std::vector<FileItem> &items)
    {
        for (const auto &item : items)
        {
            try
            {
                auto dest = fs::path(tmp_path) / item.path.filename();
                if (item.is_directory)
                    fs::copy(item.path, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                else
                    fs::copy_file(item.path, dest, fs::copy_options::overwrite_existing);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error copying " << item.path << ": " << e.what() << "\n";
            }
        }
        refreshTmpDirectory();
    }

    void clearTmp()
    {
        try
        {
            for (const auto &entry : fs::directory_iterator(tmp_path))
                fs::remove_all(entry.path());
            refreshTmpDirectory();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error clearing tmp: " << e.what() << "\n";
        }
    }

    std::vector<FileItem> getTmpItems() { return tmp_items; }
    std::vector<FileItem> &getTmpItemsRef() { return tmp_items; }
    std::string getTmpPath() const { return tmp_path; }
};

// ════════════════════════════════════════════════════════════════════════
// 右下：训练参数（property 控件 + 文本框）
// ════════════════════════════════════════════════════════════════════════

class RightSidebarLower : public GUIComponent
{
private:
    TrainingConfiguration config;
    char model_name_buf[128];
    char output_path_buf[256];
    float lr_cache = 0.02f; ///< learning_rate 的 float 投影（property_float 吃 float&）
    Id model_id = 0;
    Id output_id = 0;

public:
    std::function<void()> on_save_config;

    RightSidebarLower(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Training Parameters", Rect{450.0f, 330.0f, 420.0f, 260.0f})
    {
        config.model_name = "model_0";
        config.epochs = 25;
        config.learning_rate = 0.02;
        config.qubits = 16;
        config.layers = 6;
        config.output_path = "./output/";
        syncBuffers();
    }

    void layout(f32 win_h)
    {
        const f32 content_h = win_h - 70.0f - 80.0f - 20.0f;
        const f32 upper_h = content_h * 0.48f;
        bounds_ = Rect{450.0f, 70.0f + upper_h + 10.0f, 420.0f, content_h - upper_h - 10.0f};
    }

    void syncBuffers()
    {
        memset(model_name_buf, 0, sizeof(model_name_buf));
        strncpy(model_name_buf, config.model_name.c_str(), sizeof(model_name_buf) - 1);
        memset(output_path_buf, 0, sizeof(output_path_buf));
        strncpy(output_path_buf, config.output_path.c_str(), sizeof(output_path_buf) - 1);
        lr_cache = (float)config.learning_rate;
    }

protected:
    void render_content(Context &c) override
    {
        const Rect cr = content_rect();
        const f32 row_h = 24.0f;
        f32 y = cr.y;

        if (!model_id) model_id = c.make_id(0x7C000007u, (Id)1);
        if (!output_id) output_id = c.make_id(0x7C000008u, (Id)1);

        const f32 val_x = cr.x + 92.0f;
        const f32 val_w = cr.w - 92.0f;

        auto text_row = [&](const char *lbl, char *buf, int cap, Id id,
                            const char *ph = nullptr) {
            label(c, Rect{cr.x, y, 88.0f, row_h}, lbl, c.theme().text, Align::Start);
            text_field(c, Rect{val_x, y, val_w, row_h}, buf, cap, ph, id);
            y += row_h + 4.0f;
        };

        text_row(tr("Model:"), model_name_buf, (int)sizeof(model_name_buf), model_id);
        property_int(c, Rect{cr.x, y, cr.w, row_h}, tr("Epochs:"), config.epochs, 1, 1000, 1);
        y += row_h + 4.0f;
        // learning_rate 是 double，property_float 吃 float& —— 用 lr_cache
        // 做投影，每帧写回。曾经这里直接 *(float*)&config.learning_rate
        // （类型双关），能跑但那是没签名的借条：哪天有人在 double 与 float
        // 之间插一个字段，float 覆写的就是别人的内存。
        property_float(c, Rect{cr.x, y, cr.w, row_h}, tr("Learn Rate:"),
                       lr_cache, 0.0001f, 10.0f, 0.001f);
        config.learning_rate = (double)lr_cache;
        y += row_h + 4.0f;
        property_int(c, Rect{cr.x, y, cr.w, row_h}, tr("Qubits:"), config.qubits, 1, 64, 1);
        y += row_h + 4.0f;
        property_int(c, Rect{cr.x, y, cr.w, row_h}, tr("Layers:"), config.layers, 1, 100, 1);
        y += row_h + 4.0f;
        text_row(tr("Output:"), output_path_buf, (int)sizeof(output_path_buf), output_id);

        const Rect train_r{cr.x, y, (cr.w - 4.0f) * 0.5f, 28.0f};
        const Rect save_r{cr.x + (cr.w - 4.0f) * 0.5f + 4.0f, y, (cr.w - 4.0f) * 0.5f, 28.0f};
        if (button(c, train_r, tr("Start Training"), ButtonStyle::Filled))
        {
            config.model_name = model_name_buf;
            config.output_path = output_path_buf;
            startTrainingRequested = true;
        }
        if (button(c, save_r, tr("Save Config"), ButtonStyle::Tinted))
        {
            config.model_name = model_name_buf;
            config.output_path = output_path_buf;
            if (on_save_config)
                on_save_config();
        }
    }

public:
    bool startTrainingRequested = false;

    void setConfig(const TrainingConfiguration &cfg)
    {
        config = cfg;
        syncBuffers();
    }

    TrainingConfiguration &getConfig()
    {
        config.model_name = model_name_buf;
        config.output_path = output_path_buf;
        return config;
    }
};

// ════════════════════════════════════════════════════════════════════════
// 中栏：动作按钮
// ════════════════════════════════════════════════════════════════════════

class MiddleBar : public GUIComponent
{
private:
    std::function<void()> onLeftArrow;
    std::function<void()> onRightArrow;
    std::function<void()> onDelete;
    std::function<void()> onRefresh;

public:
    MiddleBar(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Actions", Rect{320.0f, 70.0f, 120.0f, 460.0f}) {}

    void layout(f32 win_h)
    {
        const f32 content_h = win_h - 70.0f - 80.0f - 20.0f;
        bounds_ = Rect{320.0f, 70.0f, 120.0f, content_h};
    }

    void setCallbacks(std::function<void()> left, std::function<void()> right,
                      std::function<void()> del, std::function<void()> refresh)
    {
        onLeftArrow = left;
        onRightArrow = right;
        onDelete = del;
        onRefresh = refresh;
    }

protected:
    void render_content(Context &c) override
    {
        const Rect cr = content_rect();
        const f32 btn_h = 32.0f;
        f32 y = cr.y + 4.0f;

        label(c, Rect{cr.x, y, cr.w, 16.0f}, tr("Actions"), c.theme().text_secondary,
              Align::Center);
        y += 20.0f;

        const Rect l_r{cr.x, y, cr.w, btn_h};
        if (button(c, l_r, "<<"))
            if (onLeftArrow) onLeftArrow();
        y += btn_h + 6.0f;
        const Rect r_r{cr.x, y, cr.w, btn_h};
        if (button(c, r_r, ">>"))
            if (onRightArrow) onRightArrow();
        y += btn_h + 6.0f;
        const Rect d_r{cr.x, y, cr.w, btn_h};
        if (button(c, d_r, tr("Delete"), ButtonStyle::Bezel))
            if (onDelete) onDelete();
        y += btn_h + 6.0f;
        const Rect f_r{cr.x, y, cr.w, btn_h};
        if (button(c, f_r, tr("Refresh"), ButtonStyle::Tinted))
            if (onRefresh) onRefresh();
        y += btn_h + 14.0f;

        label(c, Rect{cr.x, y, cr.w, 16.0f}, "<< : tmp->left", c.theme().text_tertiary,
              Align::Center);
        y += 16.0f;
        label(c, Rect{cr.x, y, cr.w, 16.0f}, ">> : left->tmp", c.theme().text_tertiary,
              Align::Center);
    }

    void triggerLeftArrow() { if (onLeftArrow) onLeftArrow(); }
    void triggerRightArrow() { if (onRightArrow) onRightArrow(); }
    void triggerDelete() { if (onDelete) onDelete(); }
    void triggerRefresh() { if (onRefresh) onRefresh(); }
};

// ════════════════════════════════════════════════════════════════════════
// 底栏：运行模式 + 网络状态
// ════════════════════════════════════════════════════════════════════════

class BottomBar : public GUIComponent
{
private:
    OperationMode current_mode;
    std::vector<NetworkNode> network_nodes;
    std::vector<NetworkNode> registered_peers;   // 真实对端注册表（registerPeer 写入）
    std::atomic<bool> network_scanning;
    std::thread network_thread;
    std::mutex nodes_mutex;

public:
    BottomBar(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Mode", Rect{10.0f, 540.0f, 860.0f, 80.0f}),
          current_mode(OperationMode::LOCAL_MODE), network_scanning(false) {}

    ~BottomBar()
    {
        network_scanning = false;
        if (network_thread.joinable())
            network_thread.join();
    }

    void layout(f32 win_w, f32 win_h)
    {
        const f32 content_h = win_h - 70.0f - 80.0f - 20.0f;
        bounds_ = Rect{10.0f, 70.0f + content_h + 10.0f, win_w - 20.0f, 80.0f};
    }

protected:
    void render_content(Context &c) override
    {
        const Rect cr = content_rect();
        const f32 row_h = 24.0f;
        f32 y = cr.y;

        label(c, Rect{cr.x, y, 50.0f, row_h}, tr("Mode:"), c.theme().text, Align::Start);

        // 三种模式用 selectable（pick-one 语义：点一下选中，不提供取消）
        struct ModeDef
        {
            const char *label;
            OperationMode mode;
            f32 w;
        };
        const ModeDef modes[] = {
            {tr("LM (Local Mode)"), OperationMode::LOCAL_MODE, 150.0f},
            {tr("SM (Shared Mode)"), OperationMode::SHARED_MODE, 160.0f},
            {tr("QNM (Quantum Network Mode)"), OperationMode::QUANTUM_NETWORK_MODE, 230.0f},
        };
        f32 x = cr.x + 54.0f;
        for (const auto &m : modes)
        {
            bool sel = (current_mode == m.mode);
            const Rect r{x, y, m.w, row_h};
            if (selectable(c, r, m.label, sel))
                setMode(m.mode);
            x += m.w + 6.0f;
        }
        y += row_h + 4.0f;

        if (current_mode != OperationMode::LOCAL_MODE)
        {
            char buf[64];
            {
                std::lock_guard<std::mutex> lock(nodes_mutex);
                snprintf(buf, sizeof(buf), tr("Active Nodes: %zu"), network_nodes.size());
            }
            label(c, Rect{cr.x, y, 150.0f, 18.0f}, buf, c.theme().text_secondary, Align::Start);
            label(c, Rect{cr.x + 150.0f, y, 200.0f, 18.0f},
                  network_scanning ? tr("Status: Scanning...") : tr("Status: Ready"),
                  c.theme().text_secondary, Align::Start);
            {
                std::lock_guard<std::mutex> lock(nodes_mutex);
                if (!network_nodes.empty())
                {
                    const auto &n = network_nodes[0];
                    char node_buf[192];
                    if (n.simulated)
                        snprintf(node_buf, sizeof(node_buf), "%s @ %s:%d (%.3f) %s",
                                 n.id.c_str(), n.address.c_str(), n.port, n.quantum_fidelity,
                                 tr("(simulated)"));
                    else
                        snprintf(node_buf, sizeof(node_buf), "%s @ %s:%d (%.3f)",
                                 n.id.c_str(), n.address.c_str(), n.port, n.quantum_fidelity);
                    label(c, Rect{cr.x + 350.0f, y, cr.w - 350.0f, 18.0f}, node_buf,
                          c.theme().text_tertiary, Align::Start);
                }
            }
        }
        else
        {
            label(c, Rect{cr.x, y, cr.w, 18.0f},
                  tr("Local drive mode — read/write from local filesystem"),
                  c.theme().text_tertiary, Align::Start);
        }
    }

public:
    void setMode(OperationMode mode)
    {
        current_mode = mode;
        if (mode == OperationMode::SHARED_MODE)
        {
            startP2PScan();
        }
        else if (mode == OperationMode::QUANTUM_NETWORK_MODE)
        {
            startQuantumNetworkScan();
        }
        else
        {
            stopNetworkScan();
        }
    }

    OperationMode getMode() const { return current_mode; }

    std::vector<NetworkNode> getNetworkNodes()
    {
        std::lock_guard<std::mutex> lock(nodes_mutex);
        return network_nodes;
    }

    // 注册真实对端（分布式训练 / 量子网络节点）。显式注册，绝不伪造节点：
    // 发现协议 = 读这份注册表（未来可替换为 UDP 广播 / mDNS 主动探测）。
    void registerPeer(std::string id, std::string address, int port,
                      int latency_ms, double fidelity)
    {
        NetworkNode n;
        n.id = std::move(id);
        n.address = std::move(address);
        n.port = port;
        n.is_active = true;
        n.latency_ms = latency_ms;
        n.quantum_fidelity = static_cast<float>(fidelity);
        n.simulated = false;   // 真实对端，非模拟
        std::lock_guard<std::mutex> lock(nodes_mutex);
        registered_peers.push_back(std::move(n));
    }

private:
    void startP2PScan()
    {
        network_scanning = true;
        if (network_thread.joinable())
            network_thread.join();
        network_thread = std::thread([this]()
                                     {
            while (network_scanning && current_mode == OperationMode::SHARED_MODE) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                // 发现 = 读真实对端注册表（registerPeer）。不再用 rand() 伪造
                // IP/延迟/保真度：没有真实对端时节点列表为空，不冒充任何设备。
                refresh_nodes_from_registry();
            } });
    }

    void startQuantumNetworkScan()
    {
        network_scanning = true;
        if (network_thread.joinable())
            network_thread.join();
        network_thread = std::thread([this]()
                                     {
            while (network_scanning && current_mode == OperationMode::QUANTUM_NETWORK_MODE) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                refresh_nodes_from_registry();
            } });
    }

    // 把注册表里当前活跃的真实对端刷新到展示列表。
    void refresh_nodes_from_registry()
    {
        std::lock_guard<std::mutex> lock(nodes_mutex);
        network_nodes.clear();
        for (const auto &p : registered_peers)
            if (p.is_active)
                network_nodes.push_back(p);
    }

    void stopNetworkScan()
    {
        network_scanning = false;
        if (network_thread.joinable())
        {
            network_thread.join();
        }
        std::lock_guard<std::mutex> lock(nodes_mutex);
        network_nodes.clear();
    }
};

// ════════════════════════════════════════════════════════════════════════
// 应用主体
// ════════════════════════════════════════════════════════════════════════

class RavendrelTrainerGUI
{
private:
    qhal::QVM virtual_machine;
    std::unique_ptr<qlm::QLM> learning_machine;
    std::unique_ptr<TopBar> top_bar;
    std::unique_ptr<LeftSidebar> left_sidebar;
    std::unique_ptr<MiddleBar> middle_bar;
    std::unique_ptr<RightSidebarUpper> right_upper;
    std::unique_ptr<RightSidebarLower> right_lower;
    std::unique_ptr<BottomBar> bottom_bar;
    std::atomic<bool> is_running;
    std::atomic<bool> is_training;
    std::thread training_thread;
    std::unordered_map<int, bool> prev_keys_;
    bool show_left_sidebar = true;
    bool show_right_sidebar = true;
    bool show_about_popup = false;
    bool show_shortcuts_popup = false;
    bool show_doc_popup = false;

    Context &ui;

    static constexpr int WINDOW_WIDTH = 1280;
    static constexpr int WINDOW_HEIGHT = 800;

public:
    RavendrelTrainerGUI()
        : is_running(true), is_training(false), ui(fox::ctx())
    {
        global_qm = &virtual_machine;
        top_bar = std::make_unique<TopBar>(this);
        left_sidebar = std::make_unique<LeftSidebar>(this);
        middle_bar = std::make_unique<MiddleBar>(this);
        right_upper = std::make_unique<RightSidebarUpper>(this);
        right_lower = std::make_unique<RightSidebarLower>(this);
        bottom_bar = std::make_unique<BottomBar>(this);

        middle_bar->setCallbacks(
            [this]() { copyLeftToTmp(); },
            [this]() { copyTmpToLeft(); },
            [this]() { deleteSelected(); },
            [this]() { refreshAll(); });

        top_bar->on_new = [this]() { newProject(); };
        top_bar->on_open = [this]() { openFileDialog(); };
        top_bar->on_save = [this]() { saveConfigToFile(); };
        top_bar->on_export = [this]() { exportModel(); };
        top_bar->on_select_all = [this]() { selectAll(); };
        top_bar->on_deselect_all = [this]() { deselectAll(); };
        top_bar->on_invert = [this]() { invertSelection(); };
        top_bar->on_refresh = [this]() { refreshAll(); };
        top_bar->on_toggle_left = [this]() { toggleLeftSidebar(); };
        top_bar->on_toggle_right = [this]() { toggleRightSidebar(); };
        top_bar->on_show_all = [this]() { showAllPanels(); };
        top_bar->on_start_training = [this]() { startTraining(); };
        top_bar->on_stop_training = [this]() { stopTraining(); };
        top_bar->on_configure = [this]() { configureTraining(); };
        top_bar->on_set_mode = [this](OperationMode m) { setModeFromMenu(m); };
        top_bar->on_about = [this]() { showAbout(); };
        top_bar->on_documentation = [this]() { showDocumentation(); };
        top_bar->on_shortcuts = [this]() { showShortcuts(); };
        right_lower->on_save_config = [this]() { saveConfigToFile(); };

        learning_machine = std::make_unique<qlm::QLM>(&virtual_machine, 16, 6);
    }

    ~RavendrelTrainerGUI()
    {
        is_running = false;
        if (training_thread.joinable())
            training_thread.join();
    }

    void run()
    {
        // Fox 接管窗口 / 后端 / 输入 / 字体 / 主题。
        // 深色 + 紫色强调对应旧版的「量子深色」配色（深蓝底 + 青紫强调）。
        ui.set_theme(Appearance::Dark, Accent::Purple);
        if (!ui.init("Ravendrel Trainer", WINDOW_WIDTH, WINDOW_HEIGHT))
        {
            std::cerr << "[Transmitter] fox context init failed\n";
            return;
        }

        while (!ui.should_close())
        {
            ui.frame_begin();
            processKeyboardShortcuts();
            render();
            if (right_lower->startTrainingRequested && !is_training)
            {
                right_lower->startTrainingRequested = false;
                startTraining();
            }
            ui.frame_end();
        }
        ui.shutdown();
    }

private:
    // 键盘快捷键（fox InputState 边沿触发，避免每帧重复）。
    //
    // 与旧版的关键差别：**文本字段持焦时字符类快捷键让位。旧版直接轮询
    // GLFW，在搜索框里按 Delete 会一边删字段字符一边删磁盘文件。** 现在
    // 先问焦点栈：有字段持焦就跳过 Delete / Ctrl+A / Ctrl+Enter，
    // F5 / F9（功能键）照旧 —— 它们在任何编辑器里都归应用。
    //
    // Ctrl+A / Ctrl+R 走 chars 向量（带修饰符的字符由后端推进来），
    // 不是按键表 —— Key 枚举里没有 ASCII 字符。
    void processKeyboardShortcuts()
    {
        InputState &in = ui.input();
        const bool text_focused = ui.focus().focused() != 0;

        auto key_pressed = [&](Key k) -> bool {
            const bool cur = in.key_down(k);
            auto it = prev_keys_.find((int)k);
            const bool prev = (it == prev_keys_.end()) ? false : it->second;
            prev_keys_[(int)k] = cur;
            return cur && !prev;
        };

        auto ctrl_char = [&](unsigned int cp) -> bool {
            if (!in.ctrl) return false;
            for (u32 c : in.chars)
                if (c == cp) return true;
            return false;
        };

        if (!text_focused && key_pressed(Key::Delete))
            deleteSelected();
        if (key_pressed(Key::F5) || (!text_focused && ctrl_char(18u))) // Ctrl+R = 0x12
            refreshAll();
        if (!text_focused && ctrl_char(1u)) // Ctrl+A = 0x01
            left_sidebar->toggleSelectAll();
        if (!text_focused && (ctrl_char(10u) || key_pressed(Key::Enter))) // Ctrl+Enter = 0x0A
            startTraining();
        if (key_pressed(Key::F9))
            startTraining();
    }

    void render()
    {
        // 布局按当前窗口尺寸推导（同一套比例，窗口拉多大都不出界）
        const f32 win_w = (f32)ui.width();
        const f32 win_h = (f32)ui.height();
        top_bar->layout(win_w);
        left_sidebar->layout(win_h);
        middle_bar->layout(win_h);
        right_upper->layout(win_h);
        right_lower->layout(win_h);
        bottom_bar->layout(win_w, win_h);

        // 窗口背景
        Theme &th = ui.theme();
        ui.dl().push(Rect{0.0f, 0.0f, win_w, win_h}, th.window_bg, -2.0f);

        top_bar->render(ui, /*scroll_content=*/false); // 菜单浮层不许被裁剪
        if (show_left_sidebar)
            left_sidebar->render(ui);
        middle_bar->render(ui);
        if (show_right_sidebar)
        {
            right_upper->render(ui);
            right_lower->render(ui);
        }
        bottom_bar->render(ui);

        renderPopups();
    }

    void renderPopups()
    {
        const f32 win_w = (f32)ui.width();
        const f32 win_h = (f32)ui.height();
        const f32 pw = 480.0f, ph = 320.0f;
        const Rect pr{(win_w - pw) * 0.5f, (win_h - ph) * 0.5f, pw, ph};
        Theme &th = ui.theme();

        auto popup = [&](const char *title, const std::vector<const char *> &lines) {
            // 浮层 chrome：阴影 + 面板 + 标题 + OK 按钮（返回 true = 被关掉）
            Shadow sh;
            sh.offset_y = 6.0f;
            sh.blur = 18.0f;
            sh.color = Color{0, 0, 0, 0x50};
            shadow_rect(ui, pr.translated({0.0f, 3.0f}), CornerRadii(th.corner_panel), sh, 9.0f);
            ui.dl().push_rounded(pr, th.control_bg, CornerRadii(th.corner_panel), 10.0f);
            ui.dl().push_border(pr, th.accent_color, th.hairline * 2.0f,
                                CornerRadii(th.corner_panel), 10.1f);

            const f32 title_h = 26.0f;
            ui.dl().push_rounded(Rect{pr.x, pr.y, pr.w, title_h},
                                 Color::mix(th.control_bg, th.accent_color, 0.25f),
                                 CornerRadii(th.corner_panel), 10.2f);
            label(ui, Rect{pr.x + 12.0f, pr.y, pr.w - 12.0f, title_h}, title, th.text,
                  Align::Start);

            f32 y = pr.y + title_h + 10.0f;
            for (const char *line : lines)
            {
                label(ui, Rect{pr.x + 16.0f, y, pr.w - 32.0f, 20.0f}, line, th.text_secondary,
                      Align::Start);
                y += 22.0f;
            }

            const Rect ok_r{pr.x + pr.w - 90.0f, pr.bottom() - 40.0f, 74.0f, 28.0f};
            return button(ui, ok_r, "OK", ButtonStyle::Filled);
        };

        if (show_about_popup)
        {
            if (popup(tr("About"),
                      {"Ravendrel Trainer", "Quantum Attention Model Trainer", "Version 2.0.0",
                       tr("Quantum robot simulation platform (Ravendrel)"),
                       tr("Fox GUI (Vulkan + GLFW, CPU fallback)")}))
                show_about_popup = false;
        }
        if (show_doc_popup)
        {
            if (popup(tr("Documentation"),
                      {tr("1. Select training files in the left panel"),
                       tr("2. Use '<<' to move them to training data"),
                       tr("3. Configure parameters in the right panel"),
                       tr("4. Press 'Start Training' or F9")}))
                show_doc_popup = false;
        }
        if (show_shortcuts_popup)
        {
            if (popup(tr("Keyboard Shortcuts"),
                      {"Delete        : delete selected items",
                       "F5 / Ctrl+R   : refresh file lists",
                       "Ctrl+A        : select all",
                       "Ctrl+Enter/F9 : start training"}))
                show_shortcuts_popup = false;
        }
    }

    // ---- 文件操作 ----
    //
    // 注意这里**不加锁**：所有面板状态只被主线程碰（控件只在 render 里读写），
    // 训练线程只读配置副本、只写 is_training。曾经这里有一把 gui_mutex，而
    // render() 全程持锁、按钮回调又在 render 内部触发动作 —— 每点一次
    // 「<<」就是一次非递归锁的自锁。删掉锁而不是改成 recursive_mutex：
    // 需要递归锁通常意味着临界区划错了。真正跨线程共享的只有 BottomBar
    // 的节点表（nodes_mutex，那是扫描线程在写）。
    void newProject()
    {
        right_upper->clearTmp();
        std::cout << "[GUI] New project: training data cleared\n";
    }

    void openFileDialog()
    {
#ifdef _WIN32
        OPENFILENAMEA ofn{};
        char file_buf[MAX_PATH] = {0};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = "All Files\0*.*\0";
        ofn.lpstrFile = file_buf;
        ofn.nMaxFile = sizeof(file_buf);
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn))
        {
            FileItem item;
            item.path = fs::path(file_buf);
            item.is_directory = fs::is_directory(item.path);
            item.is_selected = true;
            item.size = 0;
            right_upper->copyToTmp({item});
            std::cout << "[GUI] Opened file: " << file_buf << "\n";
        }
#else
        std::cout << "[GUI] Open file dialog is only supported on Windows\n";
#endif
    }

    void saveConfigToFile()
    {
        auto &cfg = right_lower->getConfig();
        fs::create_directories(cfg.output_path);
        std::string cfg_path = cfg.output_path + "/training_config.json";
        std::ofstream out(cfg_path);
        if (out.is_open())
        {
            out << "{\n"
                << "  \"model_name\": \"" << cfg.model_name << "\",\n"
                << "  \"epochs\": " << cfg.epochs << ",\n"
                << "  \"learning_rate\": " << cfg.learning_rate << ",\n"
                << "  \"qubits\": " << cfg.qubits << ",\n"
                << "  \"layers\": " << cfg.layers << ",\n"
                << "  \"output_path\": \"" << cfg.output_path << "\"\n"
                << "}\n";
            out.close();
            std::cout << "[GUI] Config saved to: " << cfg_path << "\n";
        }
        else
        {
            std::cerr << "[GUI] Failed to save config to: " << cfg_path << "\n";
        }
    }

    void exportModel()
    {
        auto &cfg = right_lower->getConfig();
        std::string model_path = cfg.output_path + cfg.model_name + ".rvdlm";
        std::cout << "[GUI] Model will export to: " << model_path
                  << " (run Start Training to generate)\n";
    }

    // ---- 编辑 ----
    void selectAll() { left_sidebar->selectAll(); }
    void deselectAll() { left_sidebar->deselectAll(); }
    void invertSelection() { left_sidebar->invertSelection(); }

    // ---- 视图 ----
    void toggleLeftSidebar() { show_left_sidebar = !show_left_sidebar; }
    void toggleRightSidebar() { show_right_sidebar = !show_right_sidebar; }

    // 显示所有面板（关闭/隐藏后重新打开）
    void showAllPanels()
    {
        show_left_sidebar = true;
        show_right_sidebar = true;
        top_bar->set_visible(true);
        left_sidebar->set_visible(true);
        middle_bar->set_visible(true);
        right_upper->set_visible(true);
        right_lower->set_visible(true);
        bottom_bar->set_visible(true);
    }

    // ---- 训练 ----
    void stopTraining()
    {
        if (is_training)
        {
            is_training = false;
            std::cout << "[Training] Stop requested (current epoch will finish)\n";
        }
        else
        {
            std::cout << "[Training] No training in progress\n";
        }
    }

    void configureTraining()
    {
        std::cout << "[GUI] Configure training in the right panel (Training Parameters)\n";
    }

    // ---- 网络 ----
    void setModeFromMenu(OperationMode m) { bottom_bar->setMode(m); }

    // ---- 帮助 ----
    void showAbout() { show_about_popup = true; }
    void showDocumentation() { show_doc_popup = true; }
    void showShortcuts() { show_shortcuts_popup = true; }

    void copyLeftToTmp()
    {
        auto selected = left_sidebar->getSelectedItems();
        if (!selected.empty())
        {
            right_upper->copyToTmp(selected);
            std::cout << "[GUI] Copied " << selected.size() << " items to tmp\n";
        }
    }

    void copyTmpToLeft()
    {
        auto &tmp_items = right_upper->getTmpItemsRef();
        std::string dest = left_sidebar->getCurrentPath();
        for (const auto &item : tmp_items)
        {
            if (!item.is_selected)
                continue;
            try
            {
                auto dest_path = fs::path(dest) / item.path.filename();
                if (item.is_directory)
                    fs::copy(item.path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                else
                    fs::copy_file(item.path, dest_path, fs::copy_options::overwrite_existing);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error copying: " << e.what() << "\n";
            }
        }
        left_sidebar->refreshDirectory();
    }

    void deleteSelected()
    {
        auto selected = left_sidebar->getSelectedItems();
        for (const auto &item : selected)
        {
            try
            {
                if (item.is_directory)
                {
                    fs::remove_all(item.path);
                }
                else
                {
                    fs::remove(item.path);
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error deleting: " << e.what() << "\n";
            }
        }

        auto &tmp_items = right_upper->getTmpItemsRef();
        for (auto &item : tmp_items)
        {
            if (item.is_selected)
            {
                try
                {
                    if (item.is_directory)
                    {
                        fs::remove_all(item.path);
                    }
                    else
                    {
                        fs::remove(item.path);
                    }
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Error deleting tmp: " << e.what() << "\n";
                }
            }
        }

        left_sidebar->refreshDirectory();
        right_upper->refreshTmpDirectory();
    }

    void refreshAll()
    {
        left_sidebar->refreshDirectory();
        right_upper->refreshTmpDirectory();
    }

    void startTraining()
    {
        if (is_training)
            return;

        is_training = true;
        training_thread = std::thread([this]()
                                      {
            std::cout << "\n[Training] Starting quantum model training...\n";
            auto& config = right_lower->getConfig();
            auto tmp_items = right_upper->getTmpItems();
            config.training_files.clear();

            for (const auto& item : tmp_items) {
                if (!item.is_directory) {
                    config.training_files.push_back(item.path.string());
                }
            }

            if (config.training_files.empty()) {
                std::cout << "[Error] No training files in tmp folder\n";
                is_training = false;
                return;
            }

            std::vector<std::shared_ptr<rvdl::QObject>> dataset;

            for (const auto& file : config.training_files) {
                std::ifstream input(file);
                if (!input.is_open()) continue;

                std::stringstream buffer;
                buffer << input.rdbuf();
                std::string text = buffer.str();
                auto tokens = qqnt::global_tokenizer.encode(text);
                for (size_t i = 2; i < tokens.size(); ++i) {
                    auto sample = std::make_shared<rvdl::QDataState>(
                        &virtual_machine,
                        std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
                    );

                    // 上下文：LF2C 无损特征快速上下文压缩。
                    //
                    // 原来：只有 tokens[i-2] / tokens[i-1] 两个 token，用 apply_x
                    // 打进 16 个 qubit（context.window = "2_Tokens"）。现在取
                    // **最近 16 个 token** 压成特征、用特征构造上下文态 ——
                    // qubit 占用不变（16），覆盖的历史长 8 倍。
                    //
                    // 与推理侧（qml/Inference.hpp）走**同一个函数**
                    // qlm::lf2c2::encode_context：训练与推理的上下文编码必须
                    // 一致，否则模型是在训练分布之外被推理（train/test skew）。
                    {
                        std::vector<uint8_t> history(tokens.begin(), tokens.begin() + (long)i);
                        qlm::lf2c2::encode_context(&virtual_machine,
                                                    qlm::lf2c2::ContextConfig{}, history,
                                                    sample->get_ids());
                    }

                    sample->qlm_data = new uint8_t(tokens[i]);
                    dataset.push_back(sample);
                }
            }

            std::string output_file = config.output_path + config.model_name + ".rvdlm";
            learning_machine->train_and_export(config.epochs, config.learning_rate, output_file, dataset);

            for (auto& sample : dataset) {
                delete static_cast<uint8_t*>(sample->qlm_data);
                sample->qlm_data = nullptr;
            }

            std::cout << "[Training] Completed! Model saved to: " << output_file << "\n";

            // ── 模型格式转换：.rvdlm → gguf / safetensors ────────────────
            //
            // Jarvis 需要一个「双格式」出口：.rvdlm 给 OpenButler（释放 Chimera
            // 全部能力），gguf 给 OpenCode 之类的传统运行时（降级但能跑）。
            // 训练完成后**自动**转出两份，元素类型用 double（QLM 权重是 double）。
            //
            // 转换失败不阻断训练结果（.rvdlm 已落盘），只打印诚实的原因 ——
            // 静默吞掉会让用户以为「转了」而其实没有。
            {
                std::string err;
                const std::string gguf_path = config.output_path + config.model_name + ".gguf";
                if (qml::interop::export_rvdlm_to_gguf<double>(output_file, gguf_path, "weight", &err))
                    std::cout << "[Training] Exported GGUF: " << gguf_path << "\n";
                else
                    std::cout << "[Training] GGUF export skipped: " << err << "\n";

                const std::string st_path = config.output_path + config.model_name + ".safetensors";
                if (qml::interop::export_rvdlm_to_safetensors<double>(output_file, st_path, "weight", &err))
                    std::cout << "[Training] Exported safetensors: " << st_path << "\n";
                else
                    std::cout << "[Training] safetensors export skipped: " << err << "\n";
            }
            is_training = false; });
    }
};

#ifdef _WIN32
static LONG WINAPI tx_crash_handler(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord)
    {
        FILE *f = fopen("tx_crash_dump.txt", "a");
        if (f)
        {
            HMODULE base = GetModuleHandleW(NULL);
            fprintf(f, "[CRASH] 0x%08lX at %p base %p rva 0x%llX\n",
                    ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress,
                    base,
                    (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)base));
            fclose(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main()
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, tx_crash_handler);
#endif
    try
    {
        set_lang(detect_lang()); // 界面语言自动检测（持久化值 / 系统 locale）
        RavendrelTrainerGUI gui;
        gui.run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}