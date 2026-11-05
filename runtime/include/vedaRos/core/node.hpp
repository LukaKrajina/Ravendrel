#pragma once
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include "types.hpp"
#include "quantum_transport.hpp"

namespace vedaros
{

    using MessageCallback = std::function<void(const QMessage &)>;
    using ServiceCallback = std::function<std::vector<uint8_t>(const std::vector<uint8_t> &)>;
    using GoalCallback = std::function<void(const std::vector<uint8_t> &)>;

    // topic 约定前缀（service 请求/响应、action 目标）
    static const char *kServiceReqPrefix = "__srv_req__";
    static const char *kServiceRespPrefix = "__srv_resp__";
    static const char *kActionGoalPrefix = "__act_goal__";

    class Node
    {
    private:
        std::string name_;
        std::shared_ptr<QDDPTransport> transport_;
        Endpoint endpoint_;
        std::atomic<bool> spinning_{false};
        std::thread spin_thread_;
        std::mutex cb_mtx;
        std::unordered_map<std::string, MessageCallback> subscriptions_;
        std::unordered_map<std::string, ServiceCallback> services_;
        std::unordered_map<std::string, GoalCallback> action_servers_;

        void spin_loop()
        {
            while (spinning_.load())
            {
                auto msgs = transport_->drain_messages();
                for (const auto &msg : msgs)
                {
                    std::lock_guard<std::mutex> lock(cb_mtx);

                    // 普通订阅
                    auto sub_it = subscriptions_.find(msg.topic);
                    if (sub_it != subscriptions_.end() && sub_it->second)
                    {
                        sub_it->second(msg);
                        continue;
                    }

                    // 服务请求分发：__srv_req__<name>
                    if (msg.topic.rfind(kServiceReqPrefix, 0) == 0)
                    {
                        std::string svc = msg.topic.substr(std::strlen(kServiceReqPrefix));
                        auto s_it = services_.find(svc);
                        if (s_it != services_.end() && s_it->second)
                        {
                            std::vector<uint8_t> resp = s_it->second(msg.payload);
                            publish(std::string(kServiceRespPrefix) + svc, TypeId::Custom, resp);
                        }
                        continue;
                    }

                    // 动作目标分发：__act_goal__<name>
                    if (msg.topic.rfind(kActionGoalPrefix, 0) == 0)
                    {
                        std::string act = msg.topic.substr(std::strlen(kActionGoalPrefix));
                        auto a_it = action_servers_.find(act);
                        if (a_it != action_servers_.end() && a_it->second)
                            a_it->second(msg.payload);
                        continue;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

    public:
        Node(std::shared_ptr<QDDPTransport> transport, const std::string &ns,
             const std::string &name, bool quantum_capable = true)
            : transport_(std::move(transport)), name_(name)
        {
            endpoint_ = transport_->register_node(ns, name, quantum_capable);
        }

        ~Node()
        {
            if (spinning_.load())
            {
                spinning_.store(false);
                if (spin_thread_.joinable())
                    spin_thread_.join();
            }
        }

        const std::string &name() const { return name_; }
        const Endpoint &endpoint() const { return endpoint_; }

        // ─── 发布 / 订阅 ───────────────────────────────────────
        void publish(const std::string &topic, TypeId type, const std::vector<uint8_t> &payload)
        {
            transport_->publish(endpoint_, topic, type, payload);
        }

        void publish_quantum(const std::string &topic,
                             const std::vector<std::complex<double>> &state)
        {
            transport_->publish_quantum(endpoint_, topic, TypeId::AmplitudeVector, state);
        }

        void subscribe(const std::string &topic, MessageCallback cb)
        {
            std::lock_guard<std::mutex> lock(cb_mtx);
            subscriptions_[topic] = std::move(cb);
        }

        // ─── 服务（请求/响应）───────────────────────────────────
        void register_service(const std::string &service, ServiceCallback cb)
        {
            std::lock_guard<std::mutex> lock(cb_mtx);
            services_[service] = std::move(cb);
        }

        // ─── 动作（目标）────────────────────────────────────────
        void register_action_server(const std::string &action, GoalCallback cb)
        {
            std::lock_guard<std::mutex> lock(cb_mtx);
            action_servers_[action] = std::move(cb);
        }

        // ─── 服务客户端：请求 → 阻塞等待响应 ──────────────────────
        std::vector<uint8_t> call_service(const std::string &service,
                                          const std::vector<uint8_t> &request,
                                          int timeout_ms = 1000)
        {
            std::promise<std::vector<uint8_t>> p;
            auto fut = p.get_future();
            std::string resp_topic = std::string(kServiceRespPrefix) + service;
            {
                std::lock_guard<std::mutex> lock(cb_mtx);
                subscriptions_[resp_topic] = [&p](const QMessage &m)
                {
                    p.set_value(m.payload);
                };
            }
            publish(std::string(kServiceReqPrefix) + service, TypeId::Custom, request);
            if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready)
                return fut.get();
            return {};
        }

        // ─── 动作客户端：发送目标 ────────────────────────────────
        void send_goal(const std::string &action, const std::vector<uint8_t> &goal)
        {
            publish(std::string(kActionGoalPrefix) + action, TypeId::Custom, goal);
        }

        // ─── 执行器 ─────────────────────────────────────────────
        void spin() { spin_loop(); }

        void spin_async()
        {
            if (spinning_.exchange(true))
                return;
            spin_thread_ = std::thread(&Node::spin_loop, this);
        }
    };
}