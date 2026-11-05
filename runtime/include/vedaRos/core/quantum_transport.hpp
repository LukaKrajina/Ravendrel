#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <utility>
#include <iostream>
#include "types.hpp"

namespace vedaros
{

    // 去中心化节点端点
    struct Endpoint
    {
        std::string namespace_;
        std::string node_name;
        uint32_t node_id = 0;
        bool is_quantum_capable = false;
    };

    // 量子-经典消息
    struct QMessage
    {
        uint64_t timestamp_ns = 0;
        Endpoint publisher;
        std::string topic;
        TypeId type = TypeId::Void;
        std::vector<uint8_t> payload;                      // 经典负载
        std::vector<std::complex<double>> quantum_payload; // 量子负载
    };

    // QDDP 传输层：无中心、P2P发布
    class QDDPTransport
    {
    private:
        mutable std::mutex mtx;
        std::unordered_map<std::string, Endpoint> participants;
        std::vector<QMessage> message_log;
        std::atomic<uint64_t> clock_ns{0};

    public:
        QDDPTransport()
        {
            std::cout << "[QDDP] Decentralized quantum-classical transport online.\n";
        }

        Endpoint register_node(const std::string &ns, const std::string &name,
                               bool quantum_capable)
        {
            Endpoint ep;
            ep.namespace_ = ns;
            ep.node_name = name;
            ep.node_id = static_cast<uint32_t>(participants.size() + 1);
            ep.is_quantum_capable = quantum_capable;
            std::lock_guard<std::mutex> lock(mtx);
            participants[ns + "/" + name] = ep;
            std::cout << "[QDDP] Registered node '" << ns << "/" << name
                      << "' (id " << ep.node_id << ", "
                      << (quantum_capable ? "quantum-capable" : "classical") << ").\n";
            return ep;
        }

        void publish(const Endpoint &publisher, const std::string &topic, TypeId type,
                     const std::vector<uint8_t> &payload)
        {
            QMessage msg;
            msg.timestamp_ns = clock_ns.fetch_add(1);
            msg.publisher = publisher;
            msg.topic = topic;
            msg.type = type;
            msg.payload = payload;
            std::lock_guard<std::mutex> lock(mtx);
            message_log.push_back(std::move(msg));
        }

        void publish_quantum(const Endpoint &publisher, const std::string &topic, TypeId type,
                             const std::vector<std::complex<double>> &quantum_state)
        {
            QMessage msg;
            msg.timestamp_ns = clock_ns.fetch_add(1);
            msg.publisher = publisher;
            msg.topic = topic;
            msg.type = type;
            msg.quantum_payload = quantum_state;
            std::lock_guard<std::mutex> lock(mtx);
            message_log.push_back(std::move(msg));
        }

        std::vector<QMessage> drain_messages()
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<QMessage> out = std::move(message_log);
            message_log.clear();
            return out;
        }

        size_t participant_count() const
        {
            std::lock_guard<std::mutex> lock(mtx);
            return participants.size();
        }
    };
}