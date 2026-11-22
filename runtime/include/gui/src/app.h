#pragma once
#include <vector>
#include <memory>
#include <string>
#include "render_context.h"
#include "daemon_client.h"
#include "../components/window.h"
#include "../components/circuit_grid.h"
#include "../windows/state_vector_window.h"
#include "../windows/bloch_sphere_window.h"
#include "../windows/object_inspector_window.h"
#include "../windows/measurement_history_window.h"
#include "../windows/metrics_window.h"

namespace qgui {

    class App {
        public:
            App() = default;
            ~App() { shutdown(); }

            App(const App&) = delete;
            App& operator=(const App&) = delete;

            bool init(const char* title, int width, int height,
                    const std::string& host, int port);
            void run();
            void shutdown();

        private:
            RenderContext render_;
            DaemonClient client_;
            MetricsWindow metrics_;
            std::vector<std::unique_ptr<IWindow>> windows_;
            bool running_ = false;
    };
}