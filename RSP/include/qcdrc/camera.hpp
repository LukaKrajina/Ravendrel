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
        double fx = 600.0, fy = 600.0;
        double cx = 320.0, cy = 240.0;
    };

    struct RgbFrame
    {
        int width = 640;
        int height = 480;
        std::vector<uint8_t> pixels;
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

        RgbFrame capture()
        {
            RgbFrame frame;
            frame.width = 640;
            frame.height = 480;
            frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 3, 128);
            frame.timestamp_us = frame_counter_++ * 16666;
            return frame;
        }

        const CameraIntrinsics &intrinsics() const { return intrinsics_; }
    };

    class ICamera
    {
    public:
        virtual ~ICamera() = default;
        virtual RgbFrame capture() = 0;
        virtual bool is_open() const = 0;
        virtual std::string name() const = 0;
    };
}