#pragma once
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <limits.h>
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#include "Nuklear/nuklear.h"
#include "graph.hpp"

namespace rvdlrsp::blueprint
{

    class BlueprintEditor
    {
    public:
        explicit BlueprintEditor(NodeGraph &graph) : graph_(graph) {}

        // 每帧调用，在 Nuklear 上下文中绘制编辑器
        void render(nk_context *ctx)
        {
            if (nk_begin(ctx, "Blueprint Editor", nk_rect(20, 20, 900, 560),
                         NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE))
            {
                nk_layout_space_begin(ctx, NK_STATIC, 520, INT_MAX);
                struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                struct nk_rect region = nk_layout_space_bounds(ctx);

                handle_input(ctx, region);

                // 画连线（在节点下方）
                for (const auto &edge : graph_.edges())
                {
                    const Pin *from = graph_.find_pin(edge.from);
                    const Pin *to = graph_.find_pin(edge.to);
                    if (!from || !to)
                        continue;
                    struct nk_vec2 a = pin_pos(region, *from);
                    struct nk_vec2 b = pin_pos(region, *to);
                    nk_stroke_curve(canvas, a.x, a.y, (a.x + b.x) * 0.5f, a.y,
                                    (a.x + b.x) * 0.5f, b.y, b.x, b.y, 2.0f, nk_rgb(220, 220, 90));
                }

                // 画节点
                for (const auto &node : graph_.nodes())
                {
                    draw_node(canvas, region, ctx->style.font, node);
                }

                nk_layout_space_end(ctx);
            }
            nk_end(ctx);
        }

    private:
        NodeGraph &graph_;
        float pan_x_ = 0.0f, pan_y_ = 0.0f;
        NodeId selected_ = 0;
        NodeId dragging_ = 0;
        struct nk_vec2 drag_offset_{0, 0};
        bool pending_link_ = false;
        PinId pending_from_ = 0;
        bool panning_ = false;
        struct nk_vec2 pan_start_{0, 0};
        float pan_ox_ = 0, pan_oy_ = 0;

        struct nk_vec2 world_to_screen(const struct nk_rect &region, float x, float y) const
        {
            struct nk_vec2 p;
            p.x = region.x + x + pan_x_;
            p.y = region.y + y + pan_y_;
            return p;
        }

        struct nk_vec2 pin_pos(const struct nk_rect &region, const Pin &pin) const
        {
            const Node *node = graph_.find_node_of_pin(pin.id);
            if (!node)
                return {region.x, region.y};

            // 计算引脚在节点内的偏移
            float y_off = 24.0f;
            int idx = 0;
            if (pin.kind == PinKind::Input)
            {
                for (size_t i = 0; i < node->inputs.size(); ++i)
                    if (node->inputs[i].id == pin.id)
                    {
                        idx = static_cast<int>(i);
                        break;
                    }
                return world_to_screen(region, node->x, node->y + y_off + idx * 20.0f);
            }
            else
            {
                for (size_t i = 0; i < node->outputs.size(); ++i)
                    if (node->outputs[i].id == pin.id)
                    {
                        idx = static_cast<int>(i);
                        break;
                    }
                return world_to_screen(region, node->x + node_width(*node), node->y + y_off + idx * 20.0f);
            }
        }

        float node_width(const Node &node) const
        {
            size_t max_count = std::max(node.inputs.size(), node.outputs.size());
            (void)max_count;
            return 160.0f;
        }

        void draw_node(struct nk_command_buffer *canvas, const struct nk_rect &region,
                       const struct nk_user_font *font, const Node &node)
        {
            struct nk_vec2 tl = world_to_screen(region, node.x, node.y);
            float w = node_width(node);
            float h = 24.0f + std::max(node.inputs.size(), node.outputs.size()) * 20.0f + 4.0f;

            struct nk_rect body = nk_rect(tl.x, tl.y, w, h);
            struct nk_color body_color = (node.id == selected_) ? nk_rgb(70, 90, 130) : nk_rgb(50, 55, 65);
            nk_fill_rect(canvas, body, 4.0f, body_color);
            nk_stroke_rect(canvas, body, 4.0f, 1.5f, nk_rgb(120, 130, 150));

            // 标题
            nk_draw_text(canvas, nk_rect(tl.x + 8, tl.y + 4, w - 16, 16),
                         node.title.c_str(), static_cast<int>(node.title.size()),
                         font, nk_rgb(230, 230, 230), body_color);

            // 输入引脚
            float y_off = 24.0f;
            for (const auto &p : node.inputs)
            {
                float py = tl.y + y_off;
                nk_fill_circle(canvas, nk_rect(tl.x - 5, py - 5, 10, 10), nk_rgb(90, 200, 120));
                nk_draw_text(canvas, nk_rect(tl.x + 8, py - 8, w - 16, 16),
                             p.name.c_str(), static_cast<int>(p.name.size()),
                             font, nk_rgb(200, 200, 200), body_color);
                y_off += 20.0f;
            }
            // 输出引脚
            y_off = 24.0f;
            for (const auto &p : node.outputs)
            {
                float py = tl.y + y_off;
                nk_fill_circle(canvas, nk_rect(tl.x + w - 5, py - 5, 10, 10), nk_rgb(90, 160, 240));
                nk_draw_text(canvas, nk_rect(tl.x + 8, py - 8, w - 24, 16),
                             p.name.c_str(), static_cast<int>(p.name.size()),
                             font, nk_rgb(200, 200, 200), body_color);
                y_off += 20.0f;
            }
        }

        void handle_input(nk_context *ctx, const struct nk_rect &region)
        {
            struct nk_vec2 mouse = ctx->input.mouse.pos;

            // 画布平移（中键或右键拖拽空白）
            if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_MIDDLE, region) ||
                nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, region))
            {
                if (!hit_node(region, mouse))
                {
                    panning_ = true;
                    pan_start_ = mouse;
                    pan_ox_ = pan_x_;
                    pan_oy_ = pan_y_;
                }
            }
            if (panning_ && (ctx->input.mouse.buttons[NK_BUTTON_MIDDLE].down ||
                             ctx->input.mouse.buttons[NK_BUTTON_RIGHT].down))
            {
                pan_x_ = pan_ox_ + (mouse.x - pan_start_.x);
                pan_y_ = pan_oy_ + (mouse.y - pan_start_.y);
            }
            if (ctx->input.mouse.buttons[NK_BUTTON_MIDDLE].down == nk_false &&
                ctx->input.mouse.buttons[NK_BUTTON_RIGHT].down == nk_false)
            {
                panning_ = false;
            }

            // 节点拖拽（左键点击节点标题区）
            if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, region))
            {
                bool hit = false;
                for (const auto &n : graph_.nodes())
                {
                    struct nk_vec2 tl = world_to_screen(region, n.x, n.y);
                    struct nk_rect body = nk_rect(tl.x, tl.y, node_width(n), 24.0f);
                    if (nk_input_is_mouse_hovering_rect(&ctx->input, body))
                    {
                        selected_ = n.id;
                        dragging_ = n.id;
                        drag_offset_.x = mouse.x - tl.x;
                        drag_offset_.y = mouse.y - tl.y;
                        hit = true;
                        break;
                    }
                }
                if (!hit)
                {
                    // 检查引脚点击（连线）
                    if (handle_pin_click(region, mouse))
                    { /* 已在函数内处理 */
                    }
                    else
                    {
                        selected_ = 0;
                    }
                }
            }

            if (dragging_ && ctx->input.mouse.buttons[NK_BUTTON_LEFT].down)
            {
                Node *n = graph_.find_node(dragging_);
                if (n)
                {
                    n->x = (mouse.x - region.x - pan_x_) - drag_offset_.x;
                    n->y = (mouse.y - region.y - pan_y_) - drag_offset_.y;
                }
            }
            else
            {
                dragging_ = 0;
            }
        }

        bool hit_node(const struct nk_rect &region, const struct nk_vec2 &mouse)
        {
            for (const auto &n : graph_.nodes())
            {
                struct nk_vec2 tl = world_to_screen(region, n.x, n.y);
                float h = 24.0f + std::max(n.inputs.size(), n.outputs.size()) * 20.0f + 4.0f;
                struct nk_rect body = nk_rect(tl.x, tl.y, node_width(n), h);
                if (mouse.x >= body.x && mouse.x <= body.x + body.w &&
                    mouse.y >= body.y && mouse.y <= body.y + body.h)
                    return true;
            }
            return false;
        }

        bool handle_pin_click(const struct nk_rect &region, const struct nk_vec2 &mouse)
        {
            for (const auto &node : graph_.nodes())
            {
                for (const auto &p : node.inputs)
                {
                    struct nk_vec2 pos = pin_pos(region, p);
                    if (std::fabs(mouse.x - pos.x) < 8 && std::fabs(mouse.y - pos.y) < 8)
                    {
                        if (pending_link_)
                        {
                            graph_.connect_pins(pending_from_, p.id);
                            pending_link_ = false;
                        }
                        return true;
                    }
                }
                for (const auto &p : node.outputs)
                {
                    struct nk_vec2 pos = pin_pos(region, p);
                    if (std::fabs(mouse.x - pos.x) < 8 && std::fabs(mouse.y - pos.y) < 8)
                    {
                        pending_link_ = true;
                        pending_from_ = p.id;
                        return true;
                    }
                }
            }
            return false;
        }
    };
}