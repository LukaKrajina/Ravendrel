#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <iostream>
#include "hardware/observability.hpp"
#include "camera.hpp"
#include "mocap.hpp"
#include "control/prosthetic.hpp"

namespace rvdlrsp::qcdrc
{

    // ─── 相机设备抽象 ICamera 已下沉到 camera.hpp ───────────

    // ─── 动捕设备抽象 ──────────────────────────────────────
    class IMocap
    {
    public:
        virtual ~IMocap() = default;
        // 基于相机帧的姿态估计（视觉动捕）
        virtual Skeleton estimate(const RgbFrame &frame) = 0;
        // 直接获取最新骨架（外部设备动捕，如光学/惯性系统）
        virtual Skeleton update() { return {}; }
        virtual std::string name() const = 0;
    };

    // ─── 模拟相机（复用 RgbCamera）─────────────────────────
    class SimCamera : public ICamera
    {
    private:
        RgbCamera camera_;

    public:
        RgbFrame capture() override { return camera_.capture(); }
        bool is_open() const override { return true; }
        std::string name() const override { return "SimCamera"; }
    };

    // ─── 模拟动捕（复用 MotionCapture）────────────────────
    class SimMocap : public IMocap
    {
    private:
        MotionCapture mocap_;

    public:
        Skeleton estimate(const RgbFrame &frame) override { return mocap_.estimate(frame); }
        Skeleton update() override { return mocap_.skeleton(); }
        std::string name() const override { return "SimMocap"; }
    };

    // ─── 外部数据源动捕（真实设备驱动）─────────────────────
    // 真实动捕设备（OptiTrack / Vicon / 惯性动捕）在 SDK 回调中调用
    // push_skeleton() 注入骨架；update() 返回最新注入的骨架。
    // 这是与具体 SDK 解耦的通用接入方式。
    class ExternalMocap : public IMocap
    {
    private:
        Skeleton skeleton_;
        std::mutex mtx_;

    public:
        // 真实设备 SDK 回调：注入最新骨架
        void push_skeleton(const Skeleton &s)
        {
            std::lock_guard<std::mutex> lock(mtx_);
            skeleton_ = s;
        }

        Skeleton update() override
        {
            std::lock_guard<std::mutex> lock(mtx_);
            return skeleton_;
        }

        Skeleton estimate(const RgbFrame &frame) override
        {
            (void)frame;
            return update();
        }

        std::string name() const override { return "ExternalMocap"; }
    };

    // ─── 真实设备接入点（OpenCV）────────────────────────────
    // 当定义 RVDLRSP_USE_OPENCV 时，OpenCVCamera 通过 OpenCV VideoCapture
    // 读取真实 RGB 相机帧。未定义时退化为模拟。
#ifdef RVDLRSP_USE_OPENCV
#include <opencv2/opencv.hpp>
#include <cstring>
#include <chrono>
    class OpenCVCamera : public ICamera
    {
    private:
        cv::VideoCapture capture_;
        int device_index_ = 0;
        bool open_ = false;

    public:
        explicit OpenCVCamera(int index = 0) : device_index_(index)
        {
            open_camera();
        }

        ~OpenCVCamera() override
        {
            if (capture_.isOpened())
                capture_.release();
        }

        void open_camera()
        {
            open_ = capture_.open(device_index_);
            if (open_)
                RVDLRSP_INFO("qcdrc") << "OpenCVCamera opened device " << device_index_ << ".";
            else
                RVDLRSP_ERROR("qcdrc") << "OpenCVCamera failed to open device "
                                        << device_index_ << ".";
        }

        RgbFrame capture() override
        {
            RgbFrame frame;
            if (!capture_.isOpened())
            {
                open_ = false;
                return frame;
            }
            cv::Mat bgr;
            capture_ >> bgr;
            if (bgr.empty())
                return frame;

            cv::Mat rgb;
            cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

            frame.width = rgb.cols;
            frame.height = rgb.rows;
            frame.pixels.resize(static_cast<size_t>(rgb.total()) * rgb.channels());
            std::memcpy(frame.pixels.data(), rgb.data, frame.pixels.size());
            frame.timestamp_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            return frame;
        }

        bool is_open() const override { return capture_.isOpened(); }
        std::string name() const override { return "OpenCVCamera"; }
    };
#endif

    // ─── 相机设备类型 ──────────────────────────────────────
    enum class CameraKind
    {
        Sim,     // 模拟相机（纯色帧）
        Bionic,  // 义眼相机（脑意识注视 + 量子 RL 追踪，真实 RGB 帧）
        OpenCV   // 真实相机（需 RVDLRSP_USE_OPENCV）
    };

    // ─── 设备工厂 ──────────────────────────────────────────
    class DeviceFactory
    {
    public:
        // 创建相机设备（默认为模拟，可通过宏/参数切换真实设备）
        static std::shared_ptr<ICamera> create_camera(bool real = false)
        {
#ifdef RVDLRSP_USE_OPENCV
            if (real)
                return std::make_shared<OpenCVCamera>();
#else
            (void)real;
#endif
            return std::make_shared<SimCamera>();
        }

        // 按类型创建相机设备（义眼相机等）
        static std::shared_ptr<ICamera> create_camera(CameraKind kind)
        {
            switch (kind)
            {
            case CameraKind::Bionic:
                return std::make_shared<rvdlrsp::control::BionicEyeCamera>();
            case CameraKind::OpenCV:
#ifdef RVDLRSP_USE_OPENCV
                return std::make_shared<OpenCVCamera>();
#else
                return std::make_shared<SimCamera>();
#endif
            case CameraKind::Sim:
            default:
                return std::make_shared<SimCamera>();
            }
        }

        static std::shared_ptr<IMocap> create_mocap(bool real = false)
        {
            if (real)
                return std::make_shared<ExternalMocap>();
            return std::make_shared<SimMocap>();
        }
    };
}