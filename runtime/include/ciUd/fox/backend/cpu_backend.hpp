#pragma once
#include "core/context.hpp"
#include "draw/instance.hpp"

// 项目提供的像素级加速器（damage 跟踪 / 核心区快速填充 / 遮挡剔除）。
// 关掉它（set_accel(false)）就退回到逐像素参考路径 —— 像素完全一致，
// 只是慢一点，因此"加速器是否改变了输出"永远可以自动判定。
#include "utils/Pa.hpp"

namespace fox
{
    class CpuBackend
    {
    public:
        /// 创建 `w` x `h` 逻辑像素的后备缓冲。分配失败或尺寸不合理时返回 false。
        bool init(int w, int h);

        /// 调整尺寸。内容不会保留（缓冲会被清空），只做尺寸校验。
        bool resize(int w, int h);

        void shutdown();

        /// 把 `dl` 光栅化到缓冲。调用方式与 Vulkan 后端一致（frame_begin/frame_end
        /// 之间），两者可互换。
        void render(const DisplayList &dl, Context &ctx);

        /// RGBA8，行主序，左上角为原点。供显示驱动（或调用方）呈现。
        const u8 *pixels() const { return color_.data(); }
        u8 *pixels() { return color_.data(); }
        int width() const { return width_; }
        int height() const { return height_; }
        size_t stride() const { return (size_t)width_ * 4; }

        /// 与左上角像素不同的像素数。单数字健康检查：0 表示什么都没画，
        /// 几十万表示完整 UI。
        size_t non_background_pixels() const;

        /// 把缓冲写成二进制 PPM（P6）。无头测试用它作证据；选 PPM 是因为不需要编码器。
        bool write_ppm(const char *path) const;

        u32 last_instance_count() const { return (u32)instances_.instances.size(); }

        /// Glass（PT_GLASS）采样模糊背景。CPU 路径没有离屏链，用未模糊的缓冲
        /// 近似——小半径下模糊版本本就收敛于此。设为 false 可完全跳过 glass，
        /// 便于看它的贡献。
        void set_glass_supported(bool on) { glass_ = on; }
        bool glass_supported() const { return glass_; }

        /// ── 像素加速（utils/Pa.hpp）────────────────────────────────────
        ///
        /// 默认开启。关掉它得到的是**逐像素参考路径**：同样的像素、同样的
        /// 合成顺序，只是不做 damage 跳过与核心区快速填充。这正是验证手段：
        /// 同一帧渲染两遍，两份缓冲必须逐字节相等。
        void set_accel(bool on) { accel_ = on; }
        bool accel() const { return accel_; }

        /// "加速生效了吗"必须能读出来。基准测试读它；测试也读它
        /// （例如静态帧的第二帧必须跳过大部分像素，否则 damage 只是在白算哈希）。
        const pa::Stats &stats() const { return stats_; }
        void reset_stats() { stats_.reset(); }
        const pa::DamageTracker &damage() const { return damage_; }

    private:
        void raster(const InstanceSet &set, Context &ctx, f32 scale);
        void blend(int x, int y, f32 r, f32 g, f32 b, f32 a);

        std::vector<u8> color_;
        InstanceSet instances_;
        int width_ = 0;
        int height_ = 0;
        bool glass_ = true;

        pa::DamageTracker damage_;
        /// 存放"本实例涉及的脏 tile"的临时缓冲。按 tracker 的整个 tile 网格
        /// 分配，逐实例查询永不溢出（见 raster() 中的说明）。
        std::vector<pa::TileRect> tile_tmp_;
        pa::Stats stats_;
        bool accel_ = true;
    };
}