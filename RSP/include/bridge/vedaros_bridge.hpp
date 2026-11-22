#pragma once
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <cstdint>
#include <iostream>
#include "hardware/observability.hpp"
#include "vedaRos/vedaRos.hpp"

namespace rvdlrsp::bridge
{


    class VedaRosBridge
    {
    private:
        std::shared_ptr<vedaros::QDDPTransport> transport_;
        std::unordered_map<std::string, std::shared_ptr<vedaros::Node>> nodes_;

    public:
        VedaRosBridge()
            : transport_(std::make_shared<vedaros::QDDPTransport>())
        {
            RVDLRSP_INFO("bridge") << "VedaROS bridge online.";
        }

        std::shared_ptr<vedaros::Node> register_robot(const std::string &ns,
                                                      const std::string &name)
        {
            auto node = std::make_shared<vedaros::Node>(transport_, ns, name, true);
            nodes_[ns + "/" + name] = node;
            RVDLRSP_INFO("bridge") << "Registered robot node '" << ns << "/" << name << "'.";
            return node;
        }

        static std::vector<uint8_t> serialize_doubles(const std::vector<double> &v)
        {
            std::vector<uint8_t> bytes(v.size() * sizeof(double));
            if (!v.empty())
                std::memcpy(bytes.data(), v.data(), v.size() * sizeof(double));
            return bytes;
        }

        static std::vector<double> deserialize_doubles(const std::vector<uint8_t> &bytes)
        {
            std::vector<double> v(bytes.size() / sizeof(double));
            if (!v.empty())
                std::memcpy(v.data(), bytes.data(), v.size() * sizeof(double));
            return v;
        }

        void publish_joint_state(const std::shared_ptr<vedaros::Node> &node,
                                 const std::vector<double> &angles)
        {
            if (node)
                node->publish("joint_state", vedaros::TypeId::Custom,
                              serialize_doubles(angles));
        }

        void subscribe_joint_command(const std::shared_ptr<vedaros::Node> &node,
                                     std::function<void(const std::vector<double> &)> cb)
        {
            if (!node)
                return;
            node->subscribe("joint_command",
                            [cb](const vedaros::QMessage &m)
                            {
                                if (cb)
                                    cb(deserialize_doubles(m.payload));
                            });
        }

        void register_ik_service(const std::shared_ptr<vedaros::Node> &node,
                                 std::function<std::vector<double>(const std::vector<double> &)> fn)
        {
            if (!node)
                return;
            node->register_service("ik", [fn](const std::vector<uint8_t> &req)
            {
                std::vector<double> in = deserialize_doubles(req);
                std::vector<double> out = fn ? fn(in) : std::vector<double>{};
                return serialize_doubles(out);
            });
        }

        std::vector<double> call_ik_service(const std::shared_ptr<vedaros::Node> &node,
                                            const std::vector<double> &request,
                                            int timeout_ms = 1000)
        {
            if (!node)
                return {};
            std::vector<uint8_t> resp = node->call_service("ik", serialize_doubles(request),
                                                           timeout_ms);
            return deserialize_doubles(resp);
        }

        size_t participant_count() const { return transport_->participant_count(); }
        std::vector<vedaros::QMessage> drain_messages() { return transport_->drain_messages(); }
        const std::shared_ptr<vedaros::QDDPTransport> &transport() const { return transport_; }
    };
}