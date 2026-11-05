#include "app.h"
#include "../i18n.hpp"
#include "../windows/settings_window.h"
#include <iostream>

namespace qgui {
    static const char* present_mode_name(VkPresentModeKHR m) {
        switch (m) {
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX (low latency)";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        default: return "UNKNOWN";
        }
    }

    bool App::init(const char* title, int width, int height,
                const std::string& host, int port) {
        // 界面语言：自动检测（持久化值 / 系统 locale）
        set_lang(detect_lang());

        if (!render_.init(title, width, height)) {
            return false;
        }

        client_.start(host, port);
        metrics_.set_present_mode(present_mode_name(render_.present_mode()));

        windows_.push_back(std::make_unique<CircuitGridWindow>());
        windows_.push_back(std::make_unique<StateVectorWindow>());
        windows_.push_back(std::make_unique<BlochSphereWindow>());
        windows_.push_back(std::make_unique<ObjectInspectorWindow>());
        windows_.push_back(std::make_unique<MeasurementHistoryWindow>());
        windows_.push_back(std::make_unique<SettingsWindow>());

        running_ = true;
        return true;
    }

    void App::run() {
        while (running_ && !render_.should_close()) {
            render_.begin_frame();

            StateSnapshot snap = client_.snapshot();
            float dt = render_.delta_time();

            metrics_.set_connected(client_.connected());
            for (auto& w : windows_) {
                w->render(render_.ctx(), snap, dt);
            }
            metrics_.render(render_.ctx(), snap, dt);

            render_.end_frame();
        }
    }

    void App::shutdown() {
        running_ = false;
        client_.stop();
        render_.shutdown();
    }
}
