#pragma once
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <complex>
#include <cstdio>
#include "QVM.hpp"
#include "CircuitTelemetry.hpp"
#include "../gui/protocol.hpp"

namespace qhal
{

    class RVDL_RT_API VisualizationService
    {
    public:
        explicit VisualizationService(IQuantumBackend *backend) : backend_(backend) {}

        ~VisualizationService() { stop(); }

        VisualizationService(const VisualizationService &) = delete;
        VisualizationService &operator=(const VisualizationService &) = delete;

        void start()
        {
            if (running_.exchange(true))
                return;
            worker_ = std::thread([this]
                                  { run(); });
        }

        void stop()
        {
            running_ = false;
            if (worker_.joinable())
                worker_.join();
        }

        qgui::StateSnapshot snapshot()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return snapshot_;
        }

    private:
        void run();

        IQuantumBackend *backend_ = nullptr;
        std::thread worker_;
        std::mutex mutex_;
        qgui::StateSnapshot snapshot_;
        std::atomic<bool> running_{false};
    };

    inline void VisualizationService::run()
    {
        uint64_t generation = 0;

        while (running_)
        {
            auto telemetry = qhal::CircuitTelemetry::get_instance().get_log();
            std::vector<qgui::GateRecord> gates;
            gates.reserve(telemetry.size());
            for (const auto &e : telemetry)
            {
                qgui::GateRecord g{};
                std::snprintf(g.name, sizeof(g.name), "%s", e.gate_name.c_str());
                g.target = e.target_qubit;
                g.control = e.control_qubit;
                g.step = static_cast<uint64_t>(e.step_index);
                gates.push_back(g);
            }

            qgui::StateSnapshot snap;
            snap.generation = generation;
            snap.backend_name = "QVM (live rvdl execution)";

            if (backend_)
            {
                snap.num_qubits = static_cast<uint32_t>(backend_->get_num_qubits());
                snap.amplitudes = backend_->get_state_vector();
            }

            snap.gates = std::move(gates);

            auto objects = qhal::CircuitTelemetry::get_instance().get_objects();
            for (const auto &oe : objects)
            {
                qgui::ObjectRecord o;
                o.type = oe.type;
                o.ids = oe.ids;
                o.theta = oe.theta;
                o.phi = oe.phi;
                snap.objects.push_back(std::move(o));
            }
            if (snap.objects.empty() && snap.num_qubits > 0)
            {
                qgui::ObjectRecord reg;
                reg.type = "QuantumRegister";
                for (uint32_t i = 0; i < snap.num_qubits; ++i)
                    reg.ids.push_back(static_cast<int>(i));
                snap.objects.push_back(std::move(reg));
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_ = std::move(snap);
            }

            ++generation;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}