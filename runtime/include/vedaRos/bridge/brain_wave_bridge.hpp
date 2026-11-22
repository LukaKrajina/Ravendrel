#pragma once
#include <memory>
#include <string>
#include <complex>
#include <utility>
#include <iostream>
#include "../core/quantum_transport.hpp"
#include "../../qbNs/qbw.hpp"

namespace vedaros::bridge
{

    class BrainWaveBridge
    {
    private:
        std::shared_ptr<QDDPTransport> transport_;
        qbns::BrainQuantumWave *wave_;

    public:
        BrainWaveBridge(std::shared_ptr<QDDPTransport> transport,
                        qbns::BrainQuantumWave *wave)
            : transport_(std::move(transport)), wave_(wave)
        {
            std::cout << "[vedaRos.bridge] Brain quantum wave bridge online.\n";
        }

        void publish_all_streams(const Endpoint &publisher)
        {
            if (!wave_)
                return;
            for (const auto &stream : wave_->list_streams())
            {
                std::vector<std::complex<double>> state;
                for (const auto &frame : stream->frames)
                {
                    if (!frame)
                        continue;
                    auto bits = frame->measure();
                    for (int b : bits)
                        state.emplace_back(static_cast<double>(b), 0.0);
                }
                transport_->publish_quantum(
                    publisher, "brain_wave/" + stream->source_label,
                    vedaros::TypeId::AmplitudeVector, state);
            }
            std::cout << "[vedaRos.bridge] Published " << wave_->stream_count()
                      << " brain quantum streams.\n";
        }

        void establish_wave_links(const Endpoint &publisher, size_t brain_node, size_t qc_node)
        {
            if (wave_)
                wave_->establish_distribution_link(brain_node, qc_node, 1.0, 0.99);
            std::cout << "[vedaRos.bridge] Linked brain node " << brain_node
                      << " to QC node " << qc_node << " via '" << publisher.namespace_
                      << "/" << publisher.node_name << "'.\n";
        }
    };
}