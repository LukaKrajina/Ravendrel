#include "../utils/A2LIntegration.hpp"
#include "../utils/A2LNet.hpp"
#include "../utils/A2LNetIo.hpp"
#include "../utils/A2LCollective.hpp"
#include "../qhal/QVM.hpp"
#include "../qlm/QLM.hpp"
#include "../qlm/DeviceResources.hpp"
#include "../qlm/TrainingData.hpp"
#include "../qlm/SparseAdjoint.hpp"
#include "../qhal/SparseAmplitude.hpp"
#include "RVDLModelFormat.hpp"
#include "ModelInterop.hpp"
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
#include <cctype>
#include <ctime>
#include <cstring>
#include <sstream>
#include <fstream>
#include <numeric>
#include <unordered_map>

#include "ciUd/fox/core/context.hpp"
#include "ciUd/fox/widget/primitives.hpp"
#include "ciUd/fox/widget/composite.hpp"
#include "ciUd/fox/input/input.hpp"
#include "../gui/i18n.hpp"

using namespace fox;

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

    bool auto_qubits = true;
    int manual_qubits = 16;
    int max_qubits = 16;

    int layers = 6;
    std::string output_path = "./output/";
    std::vector<std::string> training_files;

    bool shuffle = false;
    uint64_t seed = 42;
    int patience = 3;
    double grad_clip = 10.0;
    double weight_decay = 0.0;
    uint64_t limit_bytes = 0;
    std::string continue_path;
    std::string json_log;

    int state_backend = 0;
    float sparse_epsilon = 0.0f;
    int sparse_tile_bits = 6;
    int batch_size = 1;
    int tile_bits = 0;

    int effective_qubits() const
    {
        const int n = auto_qubits ? max_qubits : manual_qubits;
        return std::max(8, std::min(n, max_qubits));
    }
};

inline std::string training_settings_path()
{
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA"))
        return std::string(appdata) + "/ravendrel/training_settings";
    return "ravendrel.training_settings";
#else
    if (const char *home = std::getenv("HOME"))
        return std::string(home) + "/.ravendrel/training_settings";
    return ".ravendrel/training_settings";
#endif
}

inline void load_training_settings(TrainingConfiguration &cfg)
{
    std::ifstream in(training_settings_path());
    if (!in)
        return;

    std::string line;
    while (std::getline(in, line))
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string &s) {
            const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        };
        trim(key);
        trim(val);

        try
        {
            if (key == "auto_qubits")
                cfg.auto_qubits = (val != "0" && val != "false");
            else if (key == "manual_qubits")
                cfg.manual_qubits = std::stoi(val);
            else if (key == "state_backend")
                cfg.state_backend = std::max(0, std::min(2, std::stoi(val)));
            else if (key == "sparse_epsilon")
                cfg.sparse_epsilon = std::stof(val);
            else if (key == "sparse_tile_bits")
                cfg.sparse_tile_bits = std::max(0, std::min(20, std::stoi(val)));
            else if (key == "batch_size")
                cfg.batch_size = std::max(1, std::stoi(val));
            else if (key == "tile_bits")
                cfg.tile_bits = std::max(0, std::min(12, std::stoi(val)));
        }
        catch (...)
        {
        }
    }

    cfg.manual_qubits = std::max(8, std::min(cfg.manual_qubits, cfg.max_qubits));
}

inline void save_training_settings(const TrainingConfiguration &cfg)
{
    try
    {
        fs::path p = training_settings_path();
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        if (out)
            out << "auto_qubits=" << (cfg.auto_qubits ? 1 : 0) << "\n"
                << "manual_qubits=" << cfg.manual_qubits << "\n"
                << "state_backend=" << cfg.state_backend << "\n"
                << "sparse_epsilon=" << cfg.sparse_epsilon << "\n"
                << "sparse_tile_bits=" << cfg.sparse_tile_bits << "\n"
                << "batch_size=" << cfg.batch_size << "\n"
                << "tile_bits=" << cfg.tile_bits << "\n";
    }
    catch (...)
    {
    }
}

struct NetworkNode
{
    std::string id;
    std::string address;
    int port;
    bool is_active;
    int latency_ms;
    float quantum_fidelity;








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

    void render(Context &c, bool scroll_content = true)
    {
        if (!visible_)
            return;

        Theme &th = c.theme();
        const f32 title_h = 22.0f;

        if (scroll_content)
        {
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

    Rect content_rect() const
    {
        return Rect{bounds_.x + 8.0f, bounds_.y + 26.0f,
                    bounds_.w - 16.0f, bounds_.h - 34.0f};
    }

protected:
    virtual void render_content(Context &c) = 0;
};


class TopBar : public GUIComponent
{
private:
    std::string current_file;
    std::vector<std::string> recent_files;
    char search_buffer[256];
    Id search_id = 0;

public:
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
    std::function<void()> on_settings;
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

            int count = 4;
            if (i == 0) count = 4 + (int)recent_files.size();
            else if (i == 2) count = 4;
            else if (i == 3) count = 4;
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
            case 0:
                if (item(tr("New")) && on_new) on_new();
                if (item(tr("Open...")) && on_open) on_open();
                if (item(tr("Save")) && on_save) on_save();
                if (item(tr("Export")) && on_export) on_export();
                for (const auto &rf : recent_files) item(rf.c_str(), false);
                break;
            case 1:
                if (item(tr("Select All")) && on_select_all) on_select_all();
                if (item(tr("Deselect All")) && on_deselect_all) on_deselect_all();
                if (item(tr("Invert Selection")) && on_invert) on_invert();
                break;
            case 2:
                if (item(tr("Refresh")) && on_refresh) on_refresh();
                if (item(tr("Toggle Left")) && on_toggle_left) on_toggle_left();
                if (item(tr("Toggle Right")) && on_toggle_right) on_toggle_right();
                if (item(tr("Show All Panels")) && on_show_all) on_show_all();
                break;
            case 3:
                if (item(tr("Start Training")) && on_start_training) on_start_training();
                if (item(tr("Stop Training")) && on_stop_training) on_stop_training();
                if (item(tr("Configure...")) && on_configure) on_configure();
                if (item(tr("Settings")) && on_settings) on_settings();
                break;
            case 4:
                if (item(tr("Local Mode (LM)")) && on_set_mode) on_set_mode(OperationMode::LOCAL_MODE);
                if (item(tr("Shared Mode (SM)")) && on_set_mode) on_set_mode(OperationMode::SHARED_MODE);
                if (item(tr("Quantum Net (QNM)")) && on_set_mode)
                    on_set_mode(OperationMode::QUANTUM_NETWORK_MODE);
                break;
            case 5:
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
            case 6:
                if (item(tr("About QQNT")) && on_about) on_about();
                if (item(tr("Documentation")) && on_documentation) on_documentation();
                if (item(tr("Keyboard Shortcuts")) && on_shortcuts) on_shortcuts();
                break;
            }
            x += menus[i].w + 4.0f;
        }

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

        const Rect up_r{cr.x, y, 44.0f, row_h};
        if (button(c, up_r, tr("Up"), ButtonStyle::Bezel))
            navigateUp();
        label(c, Rect{cr.x + 50.0f, y, cr.w - 50.0f, row_h}, current_path.c_str(),
              c.theme().text_tertiary, Align::Start);
        y += row_h + 4.0f;

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

        const f32 list_h = cr.bottom() - y;
        if (list_h < row_h)
            return;
        const f32 content_h = (f32)items.size() * row_h;
        const Id list_id = c.make_id(0x7C000003u, (Id)1);
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
                continue;

            const Rect row_r{lc.x, ry, lc.w, row_h};

            const Id row_id = c.make_id(0x7C000004u, (Id)(i + 1));
            const bool over = c.hover(row_r, row_id);
            if (over && c.input().buttons[(int)MouseButton::Left].pressed)
                item.is_selected = item.is_selected ? 0 : 1;

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


class RightSidebarLower : public GUIComponent
{
private:
    TrainingConfiguration config;
    char model_name_buf[128];
    char output_path_buf[256];
    float lr_cache = 0.02f;
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
        config.max_qubits = static_cast<int>(qlm::training::max_training_qubits());
        config.manual_qubits = std::min(16, config.max_qubits);
        load_training_settings(config);
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
        property_float(c, Rect{cr.x, y, cr.w, row_h}, tr("Learn Rate:"),
                       lr_cache, 0.0001f, 10.0f, 0.001f);
        config.learning_rate = (double)lr_cache;
        y += row_h + 4.0f;

        char qbuf[128];
        std::snprintf(qbuf, sizeof(qbuf), tr("Qubits: %d (%s)"),
                      config.effective_qubits(),
                      config.auto_qubits ? tr("auto max") : tr("manual"));
        label(c, Rect{cr.x, y, cr.w, row_h}, qbuf, c.theme().text, Align::Start);
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

    TrainingConfiguration &configRef() { return config; }
};


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


enum class TrainingStatusKind
{
    None,
    Info,
    Progress,
    Done,
    Error
};

struct TrainingStatus
{
    TrainingStatusKind kind = TrainingStatusKind::None;

    std::string message_key;
    std::string detail;

    int epoch = 0;
    int epochs = 0;
    size_t sample = 0;
    size_t total = 0;
    double percent = 0.0;
    double loss = 0.0;
    double accuracy = 0.0;
    double speed = 0.0;
    double eta = 0.0;
    bool stopping = false;

    std::string best_loss;
    std::string last_loss;
    std::string acc;
    std::string duration;
    bool stopped = false;

    bool visible() const
    {
        return kind != TrainingStatusKind::None;
    }
};






class DistNodeFacade
{
public:
    using NetMessage = a2l::net::NetMessage;
    using PeerInfo = a2l::net::PeerInfo;
    using OnMessage = std::function<void(const NetMessage &)>;

    explicit DistNodeFacade(std::string id, size_t shards)
        : id_(std::move(id)), shards_(shards) {}

    bool start(uint16_t port)
    {
        completion_ = std::make_unique<a2l::net::DistributedNodeCompletion>(id_, shards_);
        if (completion_->start(port))
        {
            mode_ = Mode::Completion;
            reason_ = completion_->io_reason();
            return true;
        }
        reason_ = "completion node unavailable: " + completion_->error() + " -> readiness fallback";
        completion_.reset();
        readiness_ = std::make_unique<a2l::net::DistributedNode>(id_, shards_);
        if (readiness_->start(port))
        {
            mode_ = Mode::Readiness;
            return true;
        }
        reason_ = "readiness fallback failed: " + readiness_->error();
        readiness_.reset();
        mode_ = Mode::None;
        return false;
    }
    void stop()
    {
        if (completion_) completion_->stop();
        if (readiness_) readiness_->stop();
        completion_.reset(); readiness_.reset();
        mode_ = Mode::None;
    }
    bool active() const { return mode_ != Mode::None; }
    uint16_t port() const
    {
        return mode_ == Mode::Completion ? completion_->port()
             : mode_ == Mode::Readiness ? readiness_->port() : 0;
    }
    void on_message(OnMessage cb)
    {
        if (completion_) completion_->on_message(std::move(cb));
        else if (readiness_) readiness_->on_message(std::move(cb));
    }
    bool connect_peer(const std::string &host, uint16_t port, const std::string &id)
    {
        return mode_ == Mode::Completion
             ? completion_->connect_peer(host, port, id)
             : (mode_ == Mode::Readiness && readiness_->connect_peer(host, port, id));
    }
    std::vector<PeerInfo> peers() const
    {
        return mode_ == Mode::Completion ? completion_->peers()
             : mode_ == Mode::Readiness ? readiness_->peers()
             : std::vector<PeerInfo>{};
    }
    void heartbeat(uint64_t timeout_ms)
    {
        if (completion_) completion_->heartbeat(timeout_ms);
        else if (readiness_) readiness_->heartbeat(timeout_ms);
    }
    const std::string &reason() const { return reason_; }
    bool completion_active() const { return mode_ == Mode::Completion; }
private:
    enum class Mode { None, Completion, Readiness };
    std::string id_;
    size_t shards_;
    Mode mode_ = Mode::None;
    std::string reason_;
    std::unique_ptr<a2l::net::DistributedNodeCompletion> completion_;
    std::unique_ptr<a2l::net::DistributedNode> readiness_;
};

class BottomBar : public GUIComponent
{
private:
    OperationMode current_mode;
    std::vector<NetworkNode> network_nodes;
    std::vector<NetworkNode> registered_peers;
    std::atomic<bool> network_scanning;
    std::unique_ptr<a2l::ThreadPool> net_pool_;
    std::unique_ptr<DistNodeFacade> dist_node_;
    a2l::Strand net_strand_;
    std::atomic<bool> net_stop_{false};
    std::future<void> net_refresh_;
    std::mutex nodes_mutex;

    std::mutex train_mutex;
    TrainingStatus training_status_;
    bool training_active_ = false;

public:
    BottomBar(RavendrelTrainerGUI *p)
        : GUIComponent(p, "Mode", Rect{10.0f, 540.0f, 860.0f, 80.0f}),
          current_mode(OperationMode::LOCAL_MODE), network_scanning(false) {}

    ~BottomBar()
    {
        network_scanning = false;
        net_stop_ = true;
        if (net_refresh_.valid())
            net_refresh_.wait();
        if (dist_node_) dist_node_->stop();
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

        TrainingStatus status;
        bool status_active = false;
        {
            std::lock_guard<std::mutex> lock(train_mutex);
            status = training_status_;
            status_active = training_active_;
        }

        if (status.visible())
        {
            std::string line;
            switch (status.kind)
            {
            case TrainingStatusKind::Progress:
            {
                char buf[384];
                std::snprintf(
                    buf, sizeof(buf),
                    tr("Training epoch %d/%d | %zu/%zu | %.1f%% | loss %.4f | "
                       "acc %.1f%% | %.1f samp/s | ETA %.0fs"),
                    status.epoch, status.epochs, status.sample, status.total,
                    status.percent, status.loss, status.accuracy * 100.0,
                    status.speed, status.eta);
                line = buf;
                if (status.stopping)
                    line += tr(" | stopping");
                break;
            }
            case TrainingStatusKind::Done:
            {
                char buf[384];
                std::snprintf(
                    buf, sizeof(buf),
                    tr("Done | best loss %s | last loss %s | acc %s | %ss"),
                    status.best_loss.c_str(), status.last_loss.c_str(),
                    status.acc.c_str(), status.duration.c_str());
                line = buf;
                if (status.stopped)
                    line += tr(" | stopped");
                break;
            }
            case TrainingStatusKind::Error:
                line = tr(status.message_key.empty() ? "Training failed"
                                                     : status.message_key.c_str());
                if (!status.detail.empty())
                    line += ": " + status.detail;
                break;
            case TrainingStatusKind::Info:
            case TrainingStatusKind::None:
            default:
                line = tr(status.message_key.c_str());
                if (!status.detail.empty())
                    line += status.detail;
                break;
            }

            label(c, Rect{cr.x, y, cr.w, 18.0f}, line.c_str(),
                  status_active ? c.theme().text : c.theme().text_tertiary,
                  Align::Start);
        }
        else if (current_mode != OperationMode::LOCAL_MODE)
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

    void setTrainingInfo(const char *key, bool active = false)
    {
        std::lock_guard<std::mutex> lock(train_mutex);
        training_status_ = TrainingStatus{};
        training_status_.kind = TrainingStatusKind::Info;
        training_status_.message_key = key;
        training_active_ = active;
    }

    void setTrainingError(const char *key, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(train_mutex);
        training_status_ = TrainingStatus{};
        training_status_.kind = TrainingStatusKind::Error;
        training_status_.message_key = key;
        training_status_.detail = std::move(detail);
        training_active_ = false;
    }

    void setTrainingProgress(const qlm::TrainProgress &p, double percent)
    {
        std::lock_guard<std::mutex> lock(train_mutex);
        training_status_ = TrainingStatus{};
        training_status_.kind = TrainingStatusKind::Progress;
        training_status_.epoch = p.epoch;
        training_status_.epochs = p.epochs;
        training_status_.sample = p.sample_in_epoch;
        training_status_.total = p.total_samples;
        training_status_.percent = percent;
        training_status_.loss = p.epoch_loss;
        training_status_.accuracy = p.epoch_accuracy;
        training_status_.speed = p.samples_per_sec;
        training_status_.eta = p.eta_sec;
        training_status_.stopping = p.stopped;
        training_active_ = true;
    }

    void setTrainingDone(std::string best_loss, std::string last_loss,
                         std::string acc, std::string duration, bool stopped)
    {
        std::lock_guard<std::mutex> lock(train_mutex);
        training_status_ = TrainingStatus{};
        training_status_.kind = TrainingStatusKind::Done;
        training_status_.best_loss = std::move(best_loss);
        training_status_.last_loss = std::move(last_loss);
        training_status_.acc = std::move(acc);
        training_status_.duration = std::move(duration);
        training_status_.stopped = stopped;
        training_active_ = false;
    }

    std::vector<NetworkNode> getNetworkNodes()
    {
        std::lock_guard<std::mutex> lock(nodes_mutex);
        return network_nodes;
    }

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
        n.simulated = false;
        {
            std::lock_guard<std::mutex> lock(nodes_mutex);
            registered_peers.push_back(n);
        }
        if (dist_node_ && n.port > 0)
            dist_node_->connect_peer(n.address, static_cast<uint16_t>(n.port), n.id);
        if (net_pool_ && dist_node_)
            net_strand_.execute(*net_pool_, [this]() { refresh_nodes_from_registry(); });
    }

private:
    void startDistributedScan()
    {
        network_scanning = true;
        net_stop_ = false;
        stopDistributedRefresh();

        if (!net_pool_)
            net_pool_ = std::make_unique<a2l::ThreadPool>(2);
        if (!dist_node_)
        {
            dist_node_ = std::make_unique<DistNodeFacade>("transmitter", 16);
            if (!dist_node_->start(0))
            {
                std::lock_guard<std::mutex> lock(nodes_mutex);
                network_nodes.clear();
                network_scanning = false;
                return;
            }
            dist_node_->on_message([this](const a2l::net::NetMessage &m)
            {
                if (m.topic == "CHECKPOINT" || m.topic == "HEARTBEAT")
                {
                    std::lock_guard<std::mutex> lock(nodes_mutex);
                    for (auto &p : network_nodes)
                        if (p.id == m.from || p.id.empty()) p.is_active = true;
                }
            });
        }
        connect_registered_peers();

        net_refresh_ = net_pool_->submit([this]()
        {
            while (!net_stop_.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (net_stop_.load(std::memory_order_relaxed)) break;
                net_strand_.execute(*net_pool_, [this]()
                {
                    refresh_nodes_from_registry();
                }).wait();
            }
        });
    }

    void stopDistributedRefresh()
    {
        net_stop_ = true;
        if (net_refresh_.valid())
            net_refresh_.wait();
    }

    void connect_registered_peers()
    {
        if (!dist_node_) return;
        std::vector<NetworkNode> snapshot;
        {
            std::lock_guard<std::mutex> lock(nodes_mutex);
            snapshot = registered_peers;
        }
        for (const auto &p : snapshot)
        {
            if (p.port <= 0) continue;
            dist_node_->connect_peer(p.address, static_cast<uint16_t>(p.port), p.id);
        }
    }

    void startP2PScan() { startDistributedScan(); }
    void startQuantumNetworkScan() { startDistributedScan(); }

    void refresh_nodes_from_registry()
    {
        std::vector<NetworkNode> next;
        std::vector<a2l::net::PeerInfo> live;
        if (dist_node_) live = dist_node_->peers();
        {
            std::lock_guard<std::mutex> lock(nodes_mutex);
            for (const auto &p : registered_peers)
            {
                NetworkNode n = p;
                n.is_active = false;
                n.simulated = false;
                for (const auto &q : live)
                    if (q.alive && (q.id == p.id || q.port == p.port))
                    {
                        n.is_active = true;
                        n.id = q.id.empty() ? p.id : q.id;
                        break;
                    }
                next.push_back(n);
            }
            for (const auto &q : live)
            {
                bool known = false;
                for (const auto &n : next)
                    if (n.id == q.id && q.port == n.port) { known = true; break; }
                if (known || !q.alive) continue;
                NetworkNode n;
                n.id = q.id.empty() ? "peer" : q.id;
                n.address = q.host;
                n.port = q.port;
                n.is_active = true;
                n.latency_ms = 0;
                n.quantum_fidelity = 0.0f;
                n.simulated = false;
                next.push_back(n);
            }
            network_nodes = std::move(next);
        }
        if (dist_node_) dist_node_->heartbeat(30000);
    }

    void stopNetworkScan()
    {
        network_scanning = false;
        stopDistributedRefresh();
        if (dist_node_)
        {
            dist_node_->stop();
            dist_node_.reset();
        }
        std::lock_guard<std::mutex> lock(nodes_mutex);
        network_nodes.clear();
    }
};


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
    std::atomic<bool> train_stop_requested;
    std::thread training_thread;
    std::unordered_map<int, bool> prev_keys_;
    bool show_left_sidebar = true;
    bool show_right_sidebar = true;
    bool show_about_popup = false;
    bool show_shortcuts_popup = false;
    bool show_doc_popup = false;
    bool show_settings_popup = false;

    Context &ui;

    static constexpr int WINDOW_WIDTH = 1280;
    static constexpr int WINDOW_HEIGHT = 800;

public:
    RavendrelTrainerGUI()
        : is_running(true), is_training(false), train_stop_requested(false), ui(fox::ctx())
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
        top_bar->on_settings = [this]() { showSettings(); };
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
        train_stop_requested = true;
        if (training_thread.joinable())
            training_thread.join();
    }

    void run()
    {
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
        if (key_pressed(Key::F5) || (!text_focused && ctrl_char(18u)))
            refreshAll();
        if (!text_focused && ctrl_char(1u))
            left_sidebar->toggleSelectAll();
        if (!text_focused && (ctrl_char(10u) || key_pressed(Key::Enter)))
            startTraining();
        if (key_pressed(Key::F9))
            startTraining();
    }

    void render()
    {
        const f32 win_w = (f32)ui.width();
        const f32 win_h = (f32)ui.height();
        top_bar->layout(win_w);
        left_sidebar->layout(win_h);
        middle_bar->layout(win_h);
        right_upper->layout(win_h);
        right_lower->layout(win_h);
        bottom_bar->layout(win_w, win_h);

        Theme &th = ui.theme();
        ui.dl().push(Rect{0.0f, 0.0f, win_w, win_h}, th.window_bg, -2.0f);

        top_bar->render(ui, false);
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
        if (show_settings_popup)
            renderSettingsPopup();
    }

    void renderSettingsPopup()
    {
        const f32 win_w = (f32)ui.width();
        const f32 win_h = (f32)ui.height();
        const f32 pw = 540.0f, ph = 440.0f;
        const Rect pr{(win_w - pw) * 0.5f, (win_h - ph) * 0.5f, pw, ph};
        Theme &th = ui.theme();

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
        label(ui, Rect{pr.x + 12.0f, pr.y, pr.w - 12.0f, title_h}, tr("Settings"),
              th.text, Align::Start);

        const f32 pad = 16.0f;
        const f32 row_h = 24.0f;
        f32 y = pr.y + title_h + 14.0f;

        label(ui, Rect{pr.x + pad, y, 130.0f, row_h}, tr("Language"), th.text,
              Align::Start);
        const LangEntry *langs = available_langs();
        const int lang_count = available_lang_count();
        std::vector<const char *> lang_labels;
        lang_labels.reserve(static_cast<size_t>(lang_count));
        int lang_sel = 0;
        for (int k = 0; k < lang_count; ++k)
        {
            lang_labels.push_back(langs[k].label);
            if (langs[k].lang == current_lang())
                lang_sel = k;
        }
        if (combo(ui, Rect{pr.x + pad + 130.0f, y, pr.w - pad * 2.0f - 130.0f, row_h},
                  lang_labels.data(), lang_count, lang_sel))
            set_lang(langs[lang_sel].lang);
        y += row_h + 14.0f;

        TrainingConfiguration &cfg = right_lower->configRef();
        bool auto_q = cfg.auto_qubits;
        if (checkbox(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, row_h},
                     tr("Auto (use device maximum)"), auto_q))
        {
            cfg.auto_qubits = auto_q;
            save_training_settings(cfg);
        }
        y += row_h + 8.0f;

        char buf[192];
        std::snprintf(buf, sizeof(buf), tr("Device maximum: %d qubits"), cfg.max_qubits);
        label(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, 18.0f}, buf,
              th.text_secondary, Align::Start);
        y += 22.0f;

        if (cfg.auto_qubits)
        {
            std::snprintf(buf, sizeof(buf), tr("Effective qubits: %d"),
                          cfg.effective_qubits());
            label(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, 18.0f}, buf,
                  th.text_secondary, Align::Start);
            y += 26.0f;
        }
        else
        {
            int manual = cfg.manual_qubits;
            if (property_int(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, row_h},
                             tr("Manual qubits:"), manual, 8,
                             std::max(8, cfg.max_qubits), 1))
            {
                cfg.manual_qubits = manual;
                save_training_settings(cfg);
            }
            y += row_h + 8.0f;
        }

        label(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, 18.0f},
              tr("Changing qubits requires retraining. Existing models keep their own qubit count."),
              th.text_tertiary, Align::Start);
        y += 22.0f;

        label(ui, Rect{pr.x + pad, y, 130.0f, row_h}, tr("State backend"),
              th.text, Align::Start);
        const char *backend_labels[] = {
            "Auto (device adaptive)", "Dense (parity baseline)", "Sparse (compact tiles)"};
        int backend_sel = std::max(0, std::min(2, cfg.state_backend));
        if (combo(ui, Rect{pr.x + pad + 130.0f, y, pr.w - pad * 2.0f - 130.0f, row_h},
                  backend_labels, 3, backend_sel))
        {
            cfg.state_backend = backend_sel;
            save_training_settings(cfg);
        }
        y += row_h + 8.0f;

        {
            float eps = cfg.sparse_epsilon;
            if (property_float(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, row_h},
                               tr("Sparse epsilon:"), eps, 0.0f, 0.2f, 0.005f))
            {
                cfg.sparse_epsilon = eps;
                save_training_settings(cfg);
            }
            y += row_h + 8.0f;
        }

        {
            int tb = cfg.sparse_tile_bits;
            if (property_int(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, row_h},
                             tr("Sparse tile bits:"), tb, 0, 12, 1))
            {
                cfg.sparse_tile_bits = tb;
                save_training_settings(cfg);
            }
            y += row_h + 8.0f;
        }

        std::snprintf(buf, sizeof(buf),
                      "Backend=%s  eps=%.3f  tile=%d  batch=%d",
                      backend_sel == 0 ? "auto" : (backend_sel == 1 ? "dense" : "sparse"),
                      (double)cfg.sparse_epsilon, cfg.sparse_tile_bits, cfg.batch_size);
        label(ui, Rect{pr.x + pad, y, pr.w - pad * 2.0f, 18.0f}, buf,
              th.text_tertiary, Align::Start);

        if (button(ui, Rect{pr.x + pr.w - 100.0f, pr.bottom() - 40.0f, 84.0f, 28.0f},
                   tr("Close"), ButtonStyle::Filled))
            show_settings_popup = false;
    }

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
                << "  \"auto_qubits\": " << (cfg.auto_qubits ? "true" : "false") << ",\n"
                << "  \"manual_qubits\": " << cfg.manual_qubits << ",\n"
                << "  \"qubits\": " << cfg.effective_qubits() << ",\n"
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

    void selectAll() { left_sidebar->selectAll(); }
    void deselectAll() { left_sidebar->deselectAll(); }
    void invertSelection() { left_sidebar->invertSelection(); }

    void toggleLeftSidebar() { show_left_sidebar = !show_left_sidebar; }
    void toggleRightSidebar() { show_right_sidebar = !show_right_sidebar; }

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

    void stopTraining()
    {
        if (is_training)
        {
            train_stop_requested = true;
            bottom_bar->setTrainingInfo("Stopping after current sample...", true);
            std::cout << "[Training] Stop requested (stops at sample boundary)\n";
        }
        else
        {
            std::cout << "[Training] No training in progress\n";
        }
    }

    void configureTraining()
    {
        showSettings();
    }

    void setModeFromMenu(OperationMode m) { bottom_bar->setMode(m); }

    void showAbout()
    {
        show_about_popup = true;
        show_settings_popup = false;
    }
    void showDocumentation()
    {
        show_doc_popup = true;
        show_settings_popup = false;
    }
    void showShortcuts()
    {
        show_shortcuts_popup = true;
        show_settings_popup = false;
    }
    void showSettings()
    {
        show_settings_popup = true;
        show_about_popup = false;
        show_doc_popup = false;
        show_shortcuts_popup = false;
    }

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

        if (training_thread.joinable())
            training_thread.join();

        TrainingConfiguration cfg = right_lower->getConfig();
        std::vector<std::string> files;
        for (const auto &item : right_upper->getTmpItems())
            if (!item.is_directory)
                files.push_back(item.path.string());

        if (files.empty())
        {
            bottom_bar->setTrainingInfo("No training files in the tmp list", false);
            std::cout << "[Training] no training files in the tmp list\n";
            return;
        }

        const int n_q = cfg.effective_qubits();
        if (n_q < static_cast<int>(qlm::training::kMinTrainingQubits) ||
            n_q > static_cast<int>(qlm::training::kMaxTrainingQubits))
        {
            bottom_bar->setTrainingError("Unsupported qubit count", std::to_string(n_q));
            std::cout << "[Training] unsupported qubits=" << n_q
                      << " (expected "
                      << qlm::training::kMinTrainingQubits << ".."
                      << qlm::training::kMaxTrainingQubits << ")\n";
            return;
        }
        if (cfg.model_name.empty())
        {
            bottom_bar->setTrainingInfo("Model name is empty", false);
            std::cout << "[Training] model name is empty\n";
            return;
        }
        if (cfg.epochs <= 0 || !(cfg.learning_rate > 0.0))
        {
            bottom_bar->setTrainingInfo("Invalid epochs / learning rate", false);
            std::cout << "[Training] invalid epochs=" << cfg.epochs
                      << " lr=" << cfg.learning_rate << "\n";
            return;
        }

        const int layers = std::max(1, cfg.layers);
        train_stop_requested = false;
        is_training = true;
        bottom_bar->setTrainingInfo("Preparing training data...", true);

        try
        {
            training_thread = std::thread([this, cfg, files, layers, n_q]()
                                          {
                try
                {
                    std::cout << "\n[Training] Starting quantum model training...\n";

                    qlm::training::MultiFileTextCorpus corpus;
                    for (const auto &file : files)
                    {
                        const auto check = qlm::training::validate_training_text(file);
                        if (!check.error.empty())
                            throw std::runtime_error("[Training] 训练数据预检失败: " + check.error);
                        corpus.add_file(file, cfg.limit_bytes);
                    }
                    if (corpus.empty())
                        throw std::runtime_error(
                            "no readable training samples (files < 3 bytes or unreadable)");

                    const size_t total_samples = corpus.total_samples();
                    std::cout << "[Training] files=" << corpus.file_count()
                              << " samples=" << total_samples
                              << " qubits=" << n_q << " layers=" << layers << "\n";

                    while (virtual_machine.get_num_qubits() >
                           static_cast<size_t>(n_q))
                    {
                        const size_t last = virtual_machine.get_num_qubits() - 1;
                        virtual_machine.release_qubit(last);
                    }
                    virtual_machine.allocate_qubits(static_cast<size_t>(n_q));
                    learning_machine = std::make_unique<qlm::QLM>(
                        &virtual_machine, static_cast<size_t>(n_q),
                        static_cast<size_t>(layers));

                    std::vector<size_t> qubit_ids(static_cast<size_t>(n_q));
                    std::iota(qubit_ids.begin(), qubit_ids.end(), size_t(0));

                    qlm::training::SampleView current_view;
                    uint8_t current_target = 0;
                    auto sample = std::make_shared<rvdl::QDataState>(
                        &virtual_machine, qubit_ids, false);
                    sample->qlm_data = &current_target;

                    auto get_sample = [&](size_t index) -> std::shared_ptr<rvdl::QObject>
                    {
                        if (!corpus.get(index, current_view))
                            throw std::runtime_error(
                                "failed to read training sample " + std::to_string(index));
                        current_target = current_view.target;
                        return sample;
                    };

                    const size_t n_ctx = static_cast<size_t>(n_q);
                    const auto ctx_cfg = qlm::lf2c2::context_config_for_qubits(
                        static_cast<size_t>(n_q));
                    auto prepare_sample = [&](std::shared_ptr<rvdl::QObject> &)
                    {
                        std::vector<double> angles = qlm::lf2c2::context_angles(
                            ctx_cfg, current_view.window, n_ctx);
                        angles.resize(n_ctx, 0.0);
                        const auto amps =
                            qlm::training::product_state_amplitudes(
                                angles, static_cast<size_t>(n_q));
                        virtual_machine.prepare_amplitudes(
                            amps, static_cast<size_t>(n_q));
                    };

                    const std::string log_path = cfg.json_log.empty()
                        ? (cfg.output_path.empty() ? std::string(".") : cfg.output_path) +
                          "/train-log.jsonl"
                        : cfg.json_log;
                    a2l::ThreadPool log_pool(1);
                    a2l::Strand log_strand;
                    std::mutex log_done_mu;
                    std::condition_variable log_done_cv;
                    size_t log_pending = 0;
                    auto log_event = [&](const std::string &type, const std::string &payload)
                    {
                        {
                            std::lock_guard<std::mutex> lk(log_done_mu);
                            ++log_pending;
                        }
                        log_strand.execute(log_pool, [&, type, payload]()
                        {
                            std::ofstream f(log_path, std::ios::app);
                            if (f) f << "{\"type\":\"" << type << "\"" << payload << "}\n";
                            {
                                std::lock_guard<std::mutex> lk(log_done_mu);
                                if (log_pending > 0) --log_pending;
                            }
                            log_done_cv.notify_all();
                        });
                    };
                    struct LogFlushGuard
                    {
                        std::mutex &mu;
                        std::condition_variable &cv;
                        size_t &pending;
                        ~LogFlushGuard()
                        {
                            std::unique_lock<std::mutex> lk(mu);
                            cv.wait(lk, [&] { return pending == 0; });
                        }
                    } log_flush{log_done_mu, log_done_cv, log_pending};
                    log_event("config", ",\"files\":" + std::to_string(files.size()) +
                              ",\"samples\":" + std::to_string(total_samples) +
                              ",\"epochs\":" + std::to_string(cfg.epochs) +
                              ",\"lr\":" + std::to_string(cfg.learning_rate) +
                              ",\"seed\":" + std::to_string(cfg.seed));

                    std::vector<double> init_weights;
                    const std::vector<double> *init_ptr = nullptr;
                    if (!cfg.continue_path.empty())
                    {
                        auto loaded = qml::ModelExporter<double>::load(cfg.continue_path);
                        if (loaded.tensor.size() == 0)
                            throw std::runtime_error("[Training] --continue 模型没有权重");
                        init_weights.assign(loaded.tensor.data(),
                                            loaded.tensor.data() + loaded.tensor.size());
                        init_ptr = &init_weights;
                    }

                    qlm::TrainOptions options;
                    {
                        qlm::StateResources &sr = options.state_resources;
                        sr.qubits = static_cast<size_t>(cfg.effective_qubits());
                        sr.batch_size = static_cast<size_t>(std::max(1, cfg.batch_size));
                        sr.params = static_cast<size_t>(std::max(1, cfg.layers)) * (sr.qubits + 8);
                        sr.host_memory_bytes = qlm::training::available_memory_bytes();
                        sr.device_memory_bytes = 0;
                        sr.cpu_threads = std::max<unsigned>(2, std::thread::hardware_concurrency());
                        sr.gpu_available = false;
                        sr.moe_experts = 1;
                        sr.sparse_tile_bits = static_cast<size_t>(cfg.sparse_tile_bits ? cfg.sparse_tile_bits : 6);
                        sr.requested_sparse_epsilon = static_cast<double>(cfg.sparse_epsilon);
                    }
                    options.state_backend = cfg.state_backend == 1 ? qlm::StateBackend::Dense
                                          : cfg.state_backend == 2 ? qlm::StateBackend::Sparse
                                                                   : qlm::StateBackend::Auto;
                    options.sparse_epsilon = static_cast<double>(cfg.sparse_epsilon);
                    options.sparse_tile_bits = static_cast<size_t>(cfg.sparse_tile_bits ? cfg.sparse_tile_bits : 6);
                    options.batch_size = static_cast<size_t>(std::max(1, cfg.batch_size));
                    options.tile_bits = static_cast<size_t>(std::max(0, cfg.tile_bits));
                    options.device_adjoint_sparse_batch_pod =
                        &rvdl::sparse_adjoint::sparse_adjoint_batch_pod;
                    options.epochs = cfg.epochs;
                    options.lr = cfg.learning_rate;
                    options.prepare_sample = prepare_sample;
                    options.shuffle = cfg.shuffle;
                    options.seed = cfg.seed;
                    options.early_stop_patience = cfg.patience;
                    options.grad_clip = cfg.grad_clip;
                    options.weight_decay = cfg.weight_decay;
                    options.init_weights = init_ptr;
                    options.on_epoch_start = [&](int, size_t) { corpus.rewind(); };
                    options.should_stop = [this]()
                    {
                        return train_stop_requested.load(std::memory_order_relaxed);
                    };
                    options.checkpoint_every = 0;
                    options.checkpoint = [&](const numqk::Tensor<double> &t,
                                             const qlm::TrainProgress &p)
                    {
                        qml::RVDLModel<double> ckpt(t, {
                            {"checkpoint.partial", "1"},
                            {"checkpoint.epoch", std::to_string(p.epoch)},
                            {"checkpoint.samples_seen", std::to_string(p.samples_seen)},
                            {"model.dtype", "f64"}});
                        qml::ModelExporter<double>::save_atomic(
                            cfg.output_path + "/" + cfg.model_name + ".ckpt.rvdlm", ckpt);
                    };
                    options.extra_meta["train.interface"] = "transmitter_gui";
                    options.extra_meta["train.files"] = std::to_string(files.size());
                    options.extra_meta["train.fast_prepare"] = "product_state_amplitudes";
                    options.extra_meta["train.seed"] = std::to_string(cfg.seed);
                    options.extra_meta["train.shuffle"] = cfg.shuffle ? "1" : "0";
                    options.extra_meta["train.grad_clip"] = std::to_string(cfg.grad_clip);
                    options.extra_meta["train.limit_bytes"] = std::to_string(cfg.limit_bytes);
                    options.extra_meta["train.data_bytes"] = std::to_string(total_samples);

                    auto last_update = std::make_shared<std::chrono::steady_clock::time_point>(
                        std::chrono::steady_clock::now() - std::chrono::seconds(1));
                    options.progress = [this, last_update](const qlm::TrainProgress &p)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        if (!p.epoch_end && !p.stopped &&
                            std::chrono::duration<double>(now - *last_update).count() < 0.5)
                            return;
                        *last_update = now;

                        const double total_steps =
                            static_cast<double>(p.epochs) *
                            static_cast<double>(p.total_samples);
                        const double percent =
                            (total_steps > 0.0)
                                ? 100.0 * static_cast<double>(p.samples_seen) / total_steps
                                : 0.0;

                        bottom_bar->setTrainingProgress(p, percent);
                    };

                    const fs::path out_dir =
                        cfg.output_path.empty() ? fs::path(".") : fs::path(cfg.output_path);
                    std::error_code ec;
                    fs::create_directories(out_dir, ec);
                    if (ec)
                        throw std::runtime_error(
                            "[Training] 输出目录不可写 '" + out_dir.string() + "': " + ec.message());
                    const std::string output_file =
                        (out_dir / (cfg.model_name + ".rvdlm")).string();

                    std::cout << "[Training] output=" << output_file << "\n";
                    auto model = learning_machine->train_stream(
                        output_file, total_samples, get_sample, options);

                    std::cout << "[Training] Completed! Model saved to: " << output_file << "\n";
                    log_event("done", ",\"output\":\"" + output_file +
                              "\",\"params\":" + std::to_string(model.tensor.size()));

                    {
                        std::string err;
                        const std::string gguf_path =
                            (out_dir / (cfg.model_name + ".gguf")).string();
                        if (qml::interop::export_rvdlm_to_gguf<double>(
                                output_file, gguf_path, "weight", &err))
                            std::cout << "[Training] Exported GGUF: " << gguf_path << "\n";
                        else
                            std::cout << "[Training] GGUF export skipped: " << err << "\n";

                        const std::string st_path =
                            (out_dir / (cfg.model_name + ".safetensors")).string();
                        if (qml::interop::export_rvdlm_to_safetensors<double>(
                                output_file, st_path, "weight", &err))
                            std::cout << "[Training] Exported safetensors: " << st_path << "\n";
                        else
                            std::cout << "[Training] safetensors export skipped: " << err << "\n";
                    }

                    auto meta = [&model](const char *key, const char *fallback)
                    {
                        const auto it = model.metadata.find(key);
                        return (it == model.metadata.end())
                                   ? std::string(fallback)
                                   : it->second;
                    };
                    bottom_bar->setTrainingDone(
                        meta("train.best_loss", "?"),
                        meta("train.last_loss", "?"),
                        meta("train.last_accuracy", "?"),
                        meta("train.duration_sec", "?"),
                        meta("train.stopped", "0") == "1");
                }
                catch (const std::exception &e)
                {
                    bottom_bar->setTrainingError("Training failed", e.what());
                    std::cerr << "[Training] FAILED: " << e.what() << "\n";
                }
                catch (...)
                {
                    bottom_bar->setTrainingError("Training failed", "unknown exception");
                    std::cerr << "[Training] FAILED: unknown exception\n";
                }

                is_training = false;
                train_stop_requested = false;
            });
        }
        catch (const std::exception &e)
        {
            is_training = false;
            train_stop_requested = false;
            bottom_bar->setTrainingError("Cannot start training thread", e.what());
            std::cerr << "[Training] cannot start thread: " << e.what() << "\n";
        }
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
    A2L_TOUCH("Transmitter");
    try
    {
        set_lang(detect_lang());
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