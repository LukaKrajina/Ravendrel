#pragma once
// qbw.hpp — Brain Quantum Wave（脑量子波）
// 引用 Transducer（神经→量子编码）与 Rmx（分布式混合网络），
// 检索量子对象（QObject 体系）构造量子流（QuantumStream），
// 并建立脑量子波在脑节点与 QC 节点间的分布链接。

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <utility>
#include <iostream>

#include "Transducer.hpp"
#include "rmx.hpp"
#include "../../src/QObject.hpp"

namespace qbns
{
    // ─── 量子流：量子对象的时序载波 ───────────────────────────
    struct QuantumStream
    {
        uint64_t stream_id = 0;
        std::string source_label;                            // 来源脑节点标签
        std::vector<std::shared_ptr<rvdl::QObject>> frames; // 量子帧序列
        double carrier_frequency_hz = 40.0;                  // 载波频率（默认 gamma 频段）
        double coherence_time_us = 0.0;                      // 相干时间
        bool is_entangled_carrier = true;                    // 是否纠缠载波
    };
    
    // ─── 脑量子波分布链接 ─────────────────────────────────────
    struct WaveDistributionLink
    {
        uint64_t link_id = 0;
        size_t source_brain_node = 0;
        size_t target_qc_node = 0;
        double wave_amplitude = 1.0;
        double entanglement_fidelity = 1.0;
        double link_latency_ms = 0.0;
        bool is_active = true;
    };

    // ─── 脑量子波：量子流构造 + 分布链接管理 ─────────────────
    class BrainQuantumWave
    {
    private:
        qhal::IQuantumBackend *backend;
        Transducer *transducer;
        Rmx *rmx;

        mutable std::mutex stream_mutex;
        std::unordered_map<uint64_t, std::shared_ptr<QuantumStream>> streams;
        std::vector<WaveDistributionLink> links;
        std::atomic<uint64_t> next_stream_id{1};
        std::atomic<uint64_t> next_link_id{1};

    public:
        BrainQuantumWave(qhal::IQuantumBackend *backend_ptr,
                         Transducer *transducer_ptr,
                         Rmx *rmx_ptr)
            : backend(backend_ptr), transducer(transducer_ptr), rmx(rmx_ptr)
        {
            std::cout << "[QBW] Brain Quantum Wave fabric online.\n";
        }

        // 从量子对象集合构造量子流
        std::shared_ptr<QuantumStream> create_stream(
            const std::vector<std::shared_ptr<rvdl::QObject>> &objects,
            const std::string &source_label = "default-brain",
            double carrier_hz = 40.0,
            bool entangled = true)
        {
            auto stream = std::make_shared<QuantumStream>();
            stream->stream_id = next_stream_id.fetch_add(1);
            stream->source_label = source_label;
            stream->frames = objects;
            stream->carrier_frequency_hz = carrier_hz;
            stream->coherence_time_us = carrier_hz > 0.0 ? 1e6 / carrier_hz : 0.0;
            stream->is_entangled_carrier = entangled;

            std::lock_guard<std::mutex> lock(stream_mutex);
            streams[stream->stream_id] = stream;
            std::cout << "[QBW] Constructed quantum stream #" << stream->stream_id
                      << " (" << objects.size() << " frames, "
                      << carrier_hz << " Hz, source='" << source_label << "').\n";
            return stream;
        }

        // 从单个量子对象构造单帧量子流
        std::shared_ptr<QuantumStream> create_stream(std::shared_ptr<rvdl::QObject> obj,
                                                     const std::string &source_label = "default-brain",
                                                     double carrier_hz = 40.0)
        {
            return create_stream(std::vector<std::shared_ptr<rvdl::QObject>>{std::move(obj)},
                                 source_label, carrier_hz);
        }

        // 检索所有量子流
        std::vector<std::shared_ptr<QuantumStream>> list_streams() const
        {
            std::lock_guard<std::mutex> lock(stream_mutex);
            std::vector<std::shared_ptr<QuantumStream>> out;
            out.reserve(streams.size());
            for (const auto &kv : streams)
                out.push_back(kv.second);
            return out;
        }

        // 按流内索引检索量子对象
        std::shared_ptr<rvdl::QObject> retrieve_object(uint64_t stream_id, size_t frame_index) const
        {
            std::lock_guard<std::mutex> lock(stream_mutex);
            auto it = streams.find(stream_id);
            if (it == streams.end() || frame_index >= it->second->frames.size())
                return nullptr;
            return it->second->frames[frame_index];
        }

        // 建立脑量子波分布链接（脑节点 → QC 节点）
        WaveDistributionLink establish_distribution_link(size_t source_brain_node,
                                                         size_t target_qc_node,
                                                         double amplitude = 1.0,
                                                         double fidelity = 1.0)
        {
            if (rmx && rmx->get_link_latency(source_brain_node, target_qc_node) < 0.0)
                rmx->establish_link(source_brain_node, target_qc_node, 0.5);

            WaveDistributionLink link;
            link.link_id = next_link_id.fetch_add(1);
            link.source_brain_node = source_brain_node;
            link.target_qc_node = target_qc_node;
            link.wave_amplitude = amplitude;
            link.entanglement_fidelity = fidelity;
            link.link_latency_ms = rmx ? rmx->get_link_latency(source_brain_node, target_qc_node) : 0.0;
            link.is_active = true;

            std::lock_guard<std::mutex> lock(stream_mutex);
            links.push_back(link);
            std::cout << "[QBW] Distribution link #" << link.link_id
                      << ": brain " << source_brain_node << " -> QC " << target_qc_node
                      << " (fidelity " << fidelity << ").\n";
            return link;
        }

        std::vector<WaveDistributionLink> list_links() const
        {
            std::lock_guard<std::mutex> lock(stream_mutex);
            return links;
        }

        // 访问器
        Transducer *get_transducer() const { return transducer; }
        Rmx *get_rmx() const { return rmx; }
        qhal::IQuantumBackend *get_backend() const { return backend; }
        size_t stream_count() const { return streams.size(); }
        size_t link_count() const { return links.size(); }
    };
}