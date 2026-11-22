#include "ciUd/fox/core/context.hpp"
#include "ciUd/fox/widget/composite.hpp"
#include "ciUd/fox/widget/primitives.hpp"
#include "ciUd/fox/input/input.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace fox;

namespace
{
    int g_fail = 0;
    void check(bool ok, const char *what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_fail;
    }

    Context &c = ctx();

    template <typename F>
    bool frame(F &&widget)
    {
        c.frame_begin();
        const bool r = widget();
        c.frame_end();
        return r;
    }

    void idle_frame()
    {
        c.frame_begin();
        c.frame_end();
    }

    void move_to(f32 x, f32 y)
    {
        c.input().mouse_pos = {x, y};
    }

    void press_at(f32 x, f32 y)
    {
        move_to(x, y);
        c.input().buttons[(int)MouseButton::Left].pressed = true;
        c.input().buttons[(int)MouseButton::Left].down = true;
    }

    void release()
    {
        c.input().buttons[(int)MouseButton::Left].down = false;
        c.input().buttons[(int)MouseButton::Left].released = true;
    }

    void clear_buttons()
    {
        auto &b = c.input().buttons[(int)MouseButton::Left];
        b.pressed = false;
        b.down = false;
        b.released = false;
    }

    const Rect kTitle{10.0f, 10.0f, 80.0f, 24.0f};
    const Rect kTitle2{100.0f, 10.0f, 80.0f, 24.0f};
}

int main()
{
    c.init("fox widgets test", 800, 600);
    c.set_fixed_dt(1.0f / 60.0f);

    std::printf("[1] 命名键查询（k_index 越界 bug 的回归钉住）\n");
    {
        c.input().keys_pressed[(int)Key::Left] = true;
        check(c.input().key_press(Key::Left),
              "key_press(Key::Left) 为真（曾经恒假：越界读 keys_pressed[-1]）");
        c.input().keys_down[(int)Key::Up] = true;
        check(c.input().key_down(Key::Up), "key_down(Key::Up) 为真");
        c.input().keys_released[(int)Key::Escape] = true;
        check(c.input().key_released_now(Key::Escape), "key_released_now(Key::Escape) 为真");
        c.input().keys_pressed[(int)Key::Space] = true;
        check(c.input().key_press(Key::Space), "ASCII 键（Space）仍可查询");
        std::memset(c.input().keys_pressed, 0, sizeof(c.input().keys_pressed));
        std::memset(c.input().keys_down, 0, sizeof(c.input().keys_down));
        std::memset(c.input().keys_released, 0, sizeof(c.input().keys_released));
        idle_frame();
    }

    std::printf("[2] 菜单开合与项点击\n");
    {
        press_at(kTitle.x + 40.0f, kTitle.y + 12.0f);
        const bool clicked = frame([] { return menu_title(c, kTitle, "File"); });
        check(clicked, "点击标题返回 true");
        clear_buttons();
        idle_frame();

        Rect content{0, 0, 0, 0};
        frame([&] {
            content = menu_popup(c, kTitle, 3);
            return true;
        });
        check(content.w > 0.0f && content.h > 0.0f, "菜单打开：浮层内容 rect 非空");

        Rect item0{content.x, content.y, content.w, 22.0f};
        press_at(item0.x + 10.0f, item0.y + 11.0f);
        const bool item_clicked = frame([&] { return menu_item(c, item0, "New"); });
        check(item_clicked, "点击菜单项返回 true");
        clear_buttons();
        idle_frame();

        Rect after{0, 0, 0, 0};
        frame([&] {
            after = menu_popup(c, kTitle, 3);
            return true;
        });
        check(after.w == 0.0f, "选中后菜单自动关闭");
    }

    std::printf("[3] 菜单互斥（打开一个关闭其它）\n");
    {
        press_at(kTitle.x + 40.0f, kTitle.y + 12.0f);
        frame([] { return menu_title(c, kTitle, "File"); });
        clear_buttons();
        idle_frame();

        move_to(kTitle2.x + 40.0f, kTitle2.y + 12.0f);
        frame([] { return menu_title(c, kTitle2, "Edit"); });
        idle_frame();

        Rect file_pop{0, 0, 0, 0}, edit_pop{0, 0, 0, 0};
        frame([&] {
            file_pop = menu_popup(c, kTitle, 3);
            edit_pop = menu_popup(c, kTitle2, 3);
            return true;
        });
        check(file_pop.w == 0.0f, "File 已关闭（互斥）");
        check(edit_pop.w > 0.0f, "Edit 已打开（悬停切换）");
        menu_close_all(c);
        idle_frame();
    }

    std::printf("[4] 点击浮层外部关闭菜单\n");
    {
        press_at(kTitle.x + 40.0f, kTitle.y + 12.0f);
        frame([] { return menu_title(c, kTitle, "File"); });
        clear_buttons();
        idle_frame();

        press_at(700.0f, 500.0f);
        Rect pop{0, 0, 0, 0};
        frame([&] {
            pop = menu_popup(c, kTitle, 3);
            return true;
        });
        check(pop.w == 0.0f, "外部点击关闭菜单");
        clear_buttons();
        idle_frame();
    }

    std::printf("[5] 组合框选中\n");
    {
        const char *items[] = {"Alpha", "Beta", "Gamma"};
        int selected = 0;
        const Rect box{10.0f, 100.0f, 160.0f, 26.0f};

        press_at(box.x + 20.0f, box.y + 13.0f);
        frame([&] { return combo(c, box, items, 3, selected); });
        clear_buttons();
        idle_frame();

        const f32 item_h = 22.0f;
        Rect row1{box.x + 4.0f, box.y + box.h + 2.0f + 4.0f + item_h, box.w - 8.0f, item_h};
        press_at(row1.x + 10.0f, row1.y + 11.0f);
        const bool changed = frame([&] { return combo(c, box, items, 3, selected); });
        check(changed, "选中新项返回 true");
        check(selected == 1, "selected 更新为 1（Beta）");
        clear_buttons();
        idle_frame();

        press_at(box.x + 20.0f, box.y + 13.0f);
        frame([&] { return combo(c, box, items, 3, selected); });
        clear_buttons();
        idle_frame();
        press_at(row1.x + 10.0f, row1.y + 11.0f);
        const bool changed2 = frame([&] { return combo(c, box, items, 3, selected); });
        check(!changed2, "重复选中同一项不报变化");
        clear_buttons();
        idle_frame();

        int stale = 99;
        frame([&] { return combo(c, box, items, 3, stale); });
        check(stale == 0, "越界 selected 钳到第一项");
    }

    std::printf("[6] 可选列表项\n");
    {
        bool sel = false;
        const Rect row{10.0f, 200.0f, 200.0f, 24.0f};
        press_at(row.x + 50.0f, row.y + 12.0f);
        const bool clicked = frame([&] { return selectable(c, row, "Item A", sel); });
        check(clicked, "点击行返回 true");
        check(sel, "selected 置真");
        clear_buttons();
        idle_frame();
    }

    std::printf("[7] 数字属性控件\n");
    {
        int v = 50;
        const Rect row{10.0f, 300.0f, 240.0f, 24.0f};
        const f32 box_x = row.x + row.w * 0.45f + 8.0f;
        const f32 box_w = row.w - row.w * 0.45f - 8.0f;

        press_at(box_x + 10.0f, row.y + 12.0f);
        frame([&] { return property_int(c, row, "Count", v, 0, 100, 1); });
        move_to(box_x + 50.0f, row.y + 12.0f);
        const bool dragged = frame([&] { return property_int(c, row, "Count", v, 0, 100, 1); });
        check(dragged, "拖拽改变值返回 true");
        check(v > 50, "向右拖值增大");
        const int after_drag = v;
        release();
        idle_frame();

        press_at(box_x + 10.0f, row.y + 12.0f);
        frame([&] { return property_int(c, row, "Count", v, 0, 100, 1); });
        clear_buttons();
        idle_frame();
        c.input().keys_pressed[(int)Key::Up] = true;
        const bool nudged = frame([&] { return property_int(c, row, "Count", v, 0, 100, 1); });
        check(nudged, "方向键 Up 改变值返回 true");
        check(v == after_drag + 1, "Up 递增 1");
        std::memset(c.input().keys_pressed, 0, sizeof(c.input().keys_pressed));
        idle_frame();

        int vv = 98;
        press_at(box_x + 10.0f, row.y + 12.0f);
        frame([&] { return property_int(c, row, "Count", vv, 0, 100, 1); });
        move_to(box_x + 100000.0f, row.y + 12.0f);
        frame([&] { return property_int(c, row, "Count", vv, 0, 100, 1); });
        check(vv == 100, "拖拽钳位在 max");
        release();
        idle_frame();

        float fv = 0.5f;
        press_at(box_x + 10.0f, row.y + 12.0f);
        frame([&] { return property_float(c, row, "Gain", fv, 0.0f, 1.0f, 0.1f); });
        move_to(box_x + 60.0f, row.y + 12.0f);
        const bool fdragged = frame([&] { return property_float(c, row, "Gain", fv, 0.0f, 1.0f, 0.1f); });
        check(fdragged && fv > 0.5f, "float 拖拽递增");
        release();
        idle_frame();
    }

    std::printf("[8] 可滚动分组\n");
    {
        const Rect area{10.0f, 400.0f, 300.0f, 150.0f};
        const Id gid = c.make_id(0xABCDu, (Id)1);
        const f32 content_h = 600.0f;

        Rect content{0, 0, 0, 0};
        frame([&] {
            content = group_begin(c, area, "Group", gid, content_h);
            group_end(c);
            return true;
        });
        check(content.w > 0.0f, "分组内容 rect 非空");
        const f32 y0 = content.y;

        move_to(area.x + 50.0f, area.y + 60.0f);
        c.input().scroll_y = -3.0f;
        frame([&] {
            content = group_begin(c, area, "Group", gid, content_h);
            group_end(c);
            return true;
        });
        check(content.y < y0, "向下滚动内容上移（scroll 偏移生效）");
        const f32 scrolled = y0 - content.y;

        c.input().scroll_y = -1000.0f;
        frame([&] {
            content = group_begin(c, area, "Group", gid, content_h);
            group_end(c);
            return true;
        });
        const f32 max_scroll = content_h - (area.h - 22.0f - 2.0f);
        check(y0 - content.y <= max_scroll + 0.01f, "滚动钳位在 max_scroll");
        check(scrolled > 0.0f, "滚动确实发生过（非零位移）");
        idle_frame();
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "FOX-WIDGETS: 全部通过" : "FOX-WIDGETS: 失败",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}