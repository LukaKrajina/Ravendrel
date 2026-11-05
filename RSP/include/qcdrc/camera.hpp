#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <iostream>
#include "hardware/observability.hpp"

namespace rvdlrsp::qcdrc
{

    struct CameraIntrinsics
    {
        double fx = 600.0, fy = 600.0; // 焦距
        double cx = 320.0, cy = 240.0; // 主点
    };

    struct RgbFrame
    {
        int width = 640;
        int height = 480;
        std::vector<uint8_t> pixels; // RGB 交错
        uint64_t timestamp_us = 0;
    };

    class RgbCamera
    {
    private:
        CameraIntrinsics intrinsics_;
        uint64_t frame_counter_ = 0;

    public:
        explicit RgbCamera(CameraIntrinsics intrinsics = {}) : intrinsics_(intrinsics)
        {
            RVDLRSP_INFO("qcdrc") << "RGB camera online.";
        }

        // 采样一帧（模拟：纯色帧 + 时间戳递增）
        RgbFrame capture()
        {
            RgbFrame frame;
            frame.width = 640;
            frame.height = 480;
            frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 3, 128);
            frame.timestamp_us = frame_counter_++ * 16666; // ~60fps
            return frame;
        }

        const CameraIntrinsics &intrinsics() const { return intrinsics_; }
    };

    // ─── 相机设备抽象（下沉到 camera.hpp，供义眼相机等扩展复用）──
    class ICamera
    {
    public:
        virtual ~ICamera() = default;
        virtual RgbFrame capture() = 0;
        virtual bool is_open() const = 0;
        virtual std::string name() const = 0;
    };
}