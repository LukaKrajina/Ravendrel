#pragma once

#include "core/context.hpp"
#include "draw/display_list.hpp"

namespace fox
{
    struct InstanceData
    {
        f32 rect[4];
        f32 radii[4];
        f32 color[4];
        f32 color2[4];
        f32 params[4];
        f32 params2[4];
    };

    inline constexpr f32 PT_RECT = 0.0f;
    inline constexpr f32 PT_SHADOW = 1.0f;
    inline constexpr f32 PT_BORDER = 2.0f;
    inline constexpr f32 PT_GLASS = 3.0f;
    inline constexpr f32 PT_TEXT = 4.0f;
    inline constexpr f32 PT_IMAGE = 5.0f;
    inline constexpr f32 PT_PATH = 6.0f;
    inline constexpr f32 PT_ELLIPSE = 7.0f;
    inline constexpr f32 PT_TRIANGLE = 8.0f;

    struct Batch
    {
        u32 first = 0;
        u32 count = 0;
        FontId font = FONT_INVALID;
        TexId tex = TEX_NONE;
        Rect clip;
    };

    struct InstanceSet
    {
        std::vector<InstanceData> instances;
        std::vector<Batch> batches;
        bool used_glass = false;
        u32 dropped = 0;
        u32 culled = 0;

        void clear()
        {
            instances.clear();
            batches.clear();
            used_glass = false;
            dropped = 0;
            culled = 0;
        }

        size_t bytes() const { return instances.size() * sizeof(InstanceData); }
    };

    void build_instances(Context &c, const DisplayList &dl, InstanceSet &out);

}