// Fox core/context.cpp - 立即模式上下文实现

#include "context.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>

namespace fox
{

    // ── FocusStack ──────────────────────────────────────────────────────

    void FocusStack::begin_frame()
    {
        ++frame_;
        order_.clear();
        // 本帧消失的焦点控件失去焦点。
        if (!focused_ && focus_index_ >= 0) focus_index_ = -1;
    }

    void FocusStack::register_widget(Id id, const Rect &r, bool focusable)
    {
        if (!focusable) return;
        order_.push_back(Widget{id, r});
    }

    void FocusStack::set_focus(Id id)
    {
        focused_ = id;
        for (size_t i = 0; i < order_.size(); ++i)
            if (order_[i].id == id)
            {
                focus_index_ = (int)i;
                break;
            }
    }

    bool FocusStack::focus_next(const Rect &)
    {
        if (order_.empty()) return false;
        int n = (int)order_.size();
        int ni = (focus_index_ < 0) ? 0 : (focus_index_ + 1) % n;
        set_focus(order_[(size_t)ni].id);
        return true;
    }

    bool FocusStack::focus_prev(const Rect &)
    {
        if (order_.empty()) return false;
        int n = (int)order_.size();
        int ni = (focus_index_ < 0) ? n - 1 : (focus_index_ - 1 + n) % n;
        set_focus(order_[(size_t)ni].id);
        return true;
    }

    // ── Context ────────────────────────────────────────────────────────

    bool Context::init(const char *title, int width, int height)
    {
        // 标题存起来（供 fox_window_title() 读）。
        //
        // ⚠ 之前是 `(void)title;` —— 标题被**彻底丢弃**。fox_config 里有
        // title 字段、宿主也认真填了，而 Fox 这边一个字都没留，于是
        // 「窗口叫什么」只有宿主知道，Fox 内部（以及任何想读标题的代码）
        // 一无所知。
        title_ = title ? title : "Fox";
        // `width`/`height` 是逻辑单位，与控件布局用的单位一致。
        fb_w_ = (int)((f32)width * scale_ + 0.5f);
        fb_h_ = (int)((f32)height * scale_ + 0.5f);
        set_framebuffer_size(fb_w_, fb_h_);

        fonts().set_dpi(scale_);

        // 只在宿主没有提供剪贴板时才装内置的。要用自己的剪贴板，宿主必须
        // 在 init 之前装好；在这里检查才使"先装"成为硬性要求而不是碰运气。
        //
        // 没有剪贴板的目标平台上装的是 null，各调用方本来就得容忍这种情况
        // ——所以不支持的情形不需要在别处写单独的代码路径。
        if (!platform()) set_platform(make_default_platform());

        return true;
    }

    void Context::shutdown()
    {
        state_cache_.clear();
        text_edits_.clear();
        hover_stack_.clear();
        for (Texture &t : textures_) t.alive = false;
        textures_.clear();
        ++tex_generation_;
    }

    // ── 纹理 ──────────────────────────────────────────────────────────
    //
    // 注册表只持有像素数据。怎么送进 VRAM 由后端决定，这样加第二个后端
    // 不用改这一层以上的代码。

    TexId Context::create_texture(u32 w, u32 h, const u8 *rgba)
    {
        // 合理上限：16k x 16k x 4 是 1 GB 内存，达到或超过这个数的是调用方
        // 的 bug，不是真的纹理。照单全收的话，一个笔误会在很久之后变成
        // 内存不足崩溃，中间没有任何有用的诊断信息。
        const u32 kMaxDim = 16384;
        if (!w || !h || w > kMaxDim || h > kMaxDim)
        {
            std::fprintf(stderr, "[Fox] create_texture rejected %ux%u\n", w, h);
            return TEX_NONE;
        }
        if (!rgba) return TEX_NONE;

        textures_.push_back(Texture{});
        Texture &t = textures_.back();
        t.w = w;
        t.h = h;
        t.rgba.assign(rgba, rgba + (size_t)w * h * 4);
        t.dirty = true;
        t.uploaded = false;
        t.alive = true;
        t.generation = 1; // 取 1 而非默认的 0，避免默认槽位被误当成匹配项。
        ++tex_generation_;
        // TexId 从 1 开始，TEX_NONE 保留作"无纹理"。
        return (TexId)textures_.size();
    }

    bool Context::update_texture(TexId id, u32 w, u32 h, const u8 *rgba)
    {
        Texture *t = texture_mut(id);
        if (!t || !rgba || !w || !h) return false;

        // 重新分配 rgba 会挪动后端上传中途可能还在读的 buffer，所以尺寸
        // 变化总是递增 generation 并强制整体重传，而不是局部更新。
        if (w != t->w || h != t->h || t->rgba.size() != (size_t)w * h * 4)
        {
            t->w = w;
            t->h = h;
            t->rgba.assign(rgba, rgba + (size_t)w * h * 4);
        }
        else
        {
            memcpy(t->rgba.data(), rgba, t->rgba.size());
        }
        t->dirty = true;
        // ⚠ 这是单纹理的 generation，不是 tex_generation_。后端拿槽位的
        //   generation 与它比较来决定是否重传；只递增全局计数器的话，每个
        //   generation 都停在初始值，比较永远相等，update_texture() 就
        //   什么都不做。症状是纹理看起来完全静止——没有报错，没有警告。
        ++t->generation;
        ++tex_generation_;
        // 用旧像素重建的金字塔比没有金字塔更糟：采样器会把过期的层级混进
        // 新图像。丢掉它只是缩小的时候多一次走样，留着它就是画错。
        t->mips = img::MipChain{};
        return true;
    }

    TexId Context::create_texture_mipped(u32 w, u32 h, const u8 *rgba)
    {
        const TexId id = create_texture(w, h, rgba);
        if (id == TEX_NONE) return TEX_NONE;
        texture_mut(id)->mips = img::build_mips(w, h, rgba);
        return id;
    }

    TexId Context::create_texture_png(const u8 *png, size_t n, const char *name)
    {
        img::Image decoded;
        std::string err;
        if (!png || !img::decode_png(png, n, decoded, &err))
        {
            // 必须报错："图标没加载出来"和"图标是黑的"是两个不同的 bug，
            // 只有一个是调用方的问题。
            std::fprintf(stderr, "[Fox] create_texture_png%s%s failed: %s\n",
                         name ? " " : "", name ? name : "", err.empty() ? "bad input" : err.c_str());
            return TEX_NONE;
        }
        return create_texture_mipped(decoded.w, decoded.h, decoded.rgba.data());
    }

    void Context::destroy_texture(TexId id)
    {
        Texture *t = texture_mut(id);
        if (!t) return;
        t->alive = false;
        t->rgba.clear();
        t->rgba.shrink_to_fit();
        t->mips = img::MipChain{};
        t->w = t->h = 0;
        ++tex_generation_;
    }

    void Context::frame_begin()
    {
        dl_.reset();
        layout_.begin(Rect{0, 0, (f32)win_w_, (f32)win_h_}, Direction::Column, 0.0f);
        focus_.begin_frame();

        // 用单调时钟推进时间和 dt。放在这里（不是后端），动画相对
        // frame_begin 调用是确定性的。
        //
        // set_fixed_dt() 完全绕过时钟。没有它，无头渲染器循环调 frame_begin
        // 测出的 dt 只有微秒级，光栅化时每个动画都还停在起始值——画面里
        // anim = 0，跟实际状态无关。
        if (fixed_dt_ > 0.0f)
        {
            dt_ = fixed_dt_;
            time_ += dt_;
        }
        else
        {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now.time_since_epoch()).count();
            if (last_time_ == 0.0f) last_time_ = (f32)secs;
            dt_ = (f32)secs - last_time_;
            last_time_ = (f32)secs;
            time_ += dt_;
        }

        ++frame_;
        wants_redraw_ = false;

        prune_state_cache();
        // 每帧重置 hover 认领栈：它是**本帧**的绘制顺序记录，跨帧留着会把
        // 上一帧的控件算成「更下层」，于是本帧没人能拿到 hover。
        hover_stack_.clear();
    }

    void Context::frame_end()
    {
        layout_.end();
        input_.end_frame();
        // 本帧的顶层命中 = focus_ 栈里**最后注册**的那个（指针下最上层的控件）。
        //
        // 记录它是为了**下一帧**的点击仲裁 —— immediate-mode 下控件在绘制时
        // 才调用 claim_click，而它无法知道「自己上面还有没有别的控件」：
        // 绘制顺序是从下往上，下层的控件先 claim，「先到先得」会让**被面板
        // 盖住的按钮照样触发 action**。
        //
        // 用上一帧的结论来仲裁：一帧的延迟人眼看不出来，而层次关系在两帧
        // 之间是稳定的（布局真变了也只是晚一帧生效）。
        topmost_hit_ = focus_.topmost_registered();
    }

    Id Context::make_id(Id salt, const char *label) const
    {
        // 对 label 做 FNV-1a，再与 salt 折叠，与帧无关。
        u32 h = 2166136261u;
        if (label)
            for (const char *p = label; *p; ++p)
            {
                h ^= (u8)*p;
                h *= 16777619u;
            }
        return id_hash(salt, h);
    }

    WidgetState &Context::state(Id id)
    {
        auto it = state_cache_.find(id);
        if (it != state_cache_.end()) return it->second;
        return state_cache_.emplace(id, WidgetState{}).first->second;
    }

    WidgetState &Context::state_touch(Id id)
    {
        WidgetState &s = state(id);
        s.last_frame = frame_;
        s.last_time = time_;
        return s;
    }

    // ── IME ────────────────────────────────────────────────────────────
    //
    // 这些方法都在调用时解析拥有焦点的字段，没有就什么都不做。这不是
    // 防御性的填充——这是真实应用里最常见的流程：用户开始 IME 输入，
    // 没打完就点了别的窗口，IME 还是会发 commit。路由到过期的字段会把
    // 字打进用户当前正在看的地方。
    TextBuffer *Context::focused_text_buffer()
    {
        const Id fid = focus_.focused();
        // 只查看，不创建：text_edit() 对未知 id 会创建条目，若焦点 id 属于
        // 按钮，就会分配一个永远用不到的 TextBuffer 和一对撤销 vector。
        if (!has_text_edit(fid)) return nullptr;
        return &text_edit(fid);
    }

    void Context::ime_begin(const char *utf8, size_t caret_bytes)
    {
        if (TextBuffer *tb = focused_text_buffer())
            tb->composition_begin(std::string_view(utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0),
                                  caret_bytes);
    }

    void Context::ime_update(const char *utf8, size_t caret_bytes)
    {
        if (TextBuffer *tb = focused_text_buffer())
            tb->composition_update(std::string_view(utf8 ? utf8 : "", utf8 ? strlen(utf8) : 0),
                                   caret_bytes);
    }

    void Context::ime_commit()
    {
        if (TextBuffer *tb = focused_text_buffer()) tb->composition_commit();
    }

    void Context::ime_cancel()
    {
        if (TextBuffer *tb = focused_text_buffer()) tb->composition_cancel();
    }

    bool Context::ime_composing() const
    {
        const Id fid = focus_.focused();
        if (!has_text_edit(fid)) return false;
        return text_edits_.find(fid)->second.has_composition();
    }

    void Context::prune_state_cache()
    {
        // 10 秒墙上时间。按秒而不是按帧计，水位在 15 fps 和 240 fps 下含义
        // 相同——按帧数的话低帧率下缓存会平白大 16 倍。
        //
        // 清理太勤的代价是面板重开时控件动画从头来；太松的代价是无上限
        // 增长。10 秒远超任何正常的面板切换，工作集保持在屏幕上实际有的
        // 东西。
        constexpr f32 kStateStaleSeconds = 10.0f;
        const f32 cutoff = time_ - kStateStaleSeconds;

        // 两个缓存用同一水位、同一理由。文本编辑与控件状态同一节奏清理，
        // 滚出屏幕的字段丢撤销历史和丢悬停动画是同步的——不会一个释放、
        // 一个泄漏。
        for (auto it = state_cache_.begin(); it != state_cache_.end();)
        {
            if (it->second.last_time < cutoff)
                it = state_cache_.erase(it);
            else
                ++it;
        }
        for (auto it = text_edits_.begin(); it != text_edits_.end();)
        {
            if (it->second.last_time() < cutoff)
                it = text_edits_.erase(it);
            else
                ++it;
        }
    }

    TextBuffer &Context::text_edit(Id id)
    {
        auto it = text_edits_.find(id);
        if (it != text_edits_.end()) return it->second;

        TextBuffer &tb = text_edits_.emplace(id, TextBuffer{}).first->second;
        tb.advance_time(time_);
        return tb;
    }

    void Context::sync_text_from_host(TextBuffer &tb, const char *host, size_t host_len)
    {
        tb.advance_time(time_);

        // 只有确实不同才算。每帧都替换的话每帧都会丢掉撤销历史，因为编辑后
        // 宿主 buffer 会被写回、比较相等——只有宿主真改了它的那一帧，
        // 光标和选区本来就没意义，必须丢。
        const std::string_view hv(host ? host : "", host ? host_len : 0);
        if (hv == tb.text()) return;

        tb.set_text(hv);
    }

    bool Context::hover(const Rect &r, Id id)
    {
        const bool over = r.contains(input_.mouse_pos);
        WidgetState &s = state_touch(id);
        if (over)
        {
            // 后画的在上面：后来的控件把「hover 权」从先前的抢走。
            //
            // ⚠ 之前这里是 `s.hovered = over; return over;` —— 两个重叠的控件
            // 会**同时**报告 hover=true，于是被面板盖住的按钮照样高亮、照样
            // 能点。而这里承诺的是「later widgets get hover=0」。
            //
            // 认领顺序 = 绘制顺序（后画的在上层），所以最后认领的那个拿到
            // hover，被压住的那些收回。点击权一起转移 —— 否则被盖住的控件会
            // 保持按下态。
            for (const auto &e : hover_stack_)
            {
                WidgetState &under = state(e.first);
                under.hovered = false;
                under.pressed = false;
            }
            hover_stack_.push_back({id, r});
            s.hovered = true;
            focus_.register_widget(id, r, true);
            return true;
        }
        // 指针不在它上面：收回 hover（若之前被别的控件抢走过，也在这里收回）。
        s.hovered = false;
        return false;
    }

    bool Context::claim_hover(const Rect &r, Id id)
    {
        // 与 hover() 同义 —— hover() 本身已经实现了「后画的抢走 hover 权」。
        // 保留这个名字是因为它是「显式认领」的语义化写法。
        return hover(r, id);
    }

    bool Context::claim_click(const Rect &r, Id id)
    {
        const bool over = r.contains(input_.mouse_pos);
        WidgetState &s = state_touch(id);
        s.hovered = over;
        if (!over) return false;

        // 只有**最上层**的控件能吃到点击。
        //
        // 判定依据是**上一帧**的顶层命中（见 frame_end 的注释）：immediate-mode
        // 下控件在绘制时才 claim，而绘制顺序是从下往上 —— 「先到先得」会让被
        // 面板盖住的按钮照样触发 action。
        //
        // topmost_hit_ == 0 = 上一帧指针下没有任何控件（控件刚出现）→ 放行，
        // 免得「第一帧点不动」。
        if (topmost_hit_ != 0 && id != topmost_hit_) return false;

        if (input_.buttons[(int)MouseButton::Left].pressed)
        {
            s.pressed = true;
            focus_.set_focus(id);
            return true;
        }
        if (input_.buttons[(int)MouseButton::Left].released)
        {
            s.pressed = false;
            return s.pressed;
        }
        return false;
    }

    Context &ctx()
    {
        static Context c;
        return c;
    }

    // ── 字体 ──────────────────────────────────────────────────────────

    FontId Context::font(f32 pixel_size)
    {
        // 按有效大小（即逻辑大小 * DPI）作 key。
        //
        // 只按逻辑大小作 key，DPI 变化后会返回按旧密度光栅化的字面：
        // 度量正确、字形发虚。这是经典的 HiDPI bug，而且没有任何报错——
        // 文字只是发虚。
        const f32 eff = pixel_size * scale_;
        int key = (int)(eff * 10.0f + 0.5f);
        auto it = fonts_.find(key);
        if (it != fonts_.end()) return it->second;

        // `pixel_size` 按逻辑单位传入：字体管理器光栅化时自己应用 DPI，
        // 报回来的度量已经是逻辑单位，所有调用方都不用管 DPI。
        FontId id = fonts().load_system_ui(pixel_size);

        // 回退字面只在第一次请求字体时加载一次，用相同的像素大小，这样
        // 回退字形与主字体共享基线，不会在一行文字里上下跳动。
        //
        // 刻意只加载一次：load_file 按（路径，字面）缓存文件字节，但每多
        // 一个字号就多约 19 MB 字体数据和一个图集。第一次请求的字号才是
        // UI 文字实际用的，回退覆盖花在这里才值。
        if (!fallbacks_tried_)
        {
            fallbacks_tried_ = true;
            fonts().load_fallback_faces(pixel_size);
        }

        fonts_.emplace(key, id);
        return id;
    }
    GlyphAtlas *Context::font_atlas(FontId id)
    {
        return fonts().atlas(id);
    }

    void Context::set_theme(Appearance app, Accent ac)
    {
        // 主题 token 是逻辑单位，只在后端缩放一次——这里没有 DPI 相关的
        // 东西要重新应用。
        theme().init(app, ac);
    }

    // ── HiDPI ────────────────────────────────────────────────────────────
    //
    // Fox 全部用逻辑单位编写。显示列表存逻辑坐标；后端在送往 GPU 的路上
    // 缩放到设备像素（见 VkBackend::build_instances）。这样所有控件在任何
    // 密度下都只有一套数字——另一种做法（到处传设备像素、每个控件自己除）
    // 会把缩放系数散到几十处，而不是只有一处。

    void Context::set_framebuffer_size(int w, int h)
    {
        fb_w_ = w;
        fb_h_ = h;
        win_w_ = (int)((f32)w / scale_ + 0.5f);
        win_h_ = (int)((f32)h / scale_ + 0.5f);
    }

    bool Context::set_scale(f32 s)
    {
        if (s <= 0.0f) s = 1.0f;
        if (s == scale_) return false;
        scale_ = s;

        // 按旧密度光栅化的字体现在不对了。字体缓存按有效大小作 key，
        // 丢掉句柄就会强制按新密度重新加载。
        //
        // 主题刻意不动：它的 token 保持 1x 基准值，由后端在送往 GPU 的
        // 路上缩放。这里再缩放一次会让所有尺寸翻倍。
        fonts().set_dpi(s);
        fonts_.clear();

        set_framebuffer_size(fb_w_, fb_h_);
        request_redraw();
        return true;
    }

} // namespace fox