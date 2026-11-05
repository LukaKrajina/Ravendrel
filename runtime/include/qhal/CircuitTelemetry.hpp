#pragma once
#include <vector>
#include <string>
#include <mutex>

namespace qhal {
    struct GateEvent
    {
        std::string gate_name;
        int target_qubit;
        int control_qubit;
        int step_index;
    };

    // 量子对象创建事件（DiracState / BellState / QuantumRegister / basis_state）
    struct ObjectEvent
    {
        std::string type;
        std::vector<int> ids;
        double theta = 0.0;   // 任意基极角（basis_state）
        double phi = 0.0;     // 任意基方位角
    };

    class CircuitTelemetry {
    private:
        std::vector<GateEvent> event_log;
        std::vector<ObjectEvent> object_log;
        std::mutex log_mutex;
        int current_step = 0;

    public:
        static CircuitTelemetry& get_instance() {
            static CircuitTelemetry instance;
            return instance;
        }

        void log_gate(const std::string& name, int target, int control = -1) {
            std::lock_guard<std::mutex> lock(log_mutex);
            event_log.push_back({name, target, control, current_step++});
        }

        // 记录量子对象创建（theta/phi 仅对任意基态 basis_state 有意义）
        void log_object(const std::string& type, const std::vector<int>& ids,
                        double theta = 0.0, double phi = 0.0) {
            std::lock_guard<std::mutex> lock(log_mutex);
            object_log.push_back({type, ids, theta, phi});
        }

        std::vector<GateEvent> get_log() {
            std::lock_guard<std::mutex> lock(log_mutex);
            return event_log;
        }

        std::vector<ObjectEvent> get_objects() {
            std::lock_guard<std::mutex> lock(log_mutex);
            return object_log;
        }

        void clear() {
            std::lock_guard<std::mutex> lock(log_mutex);
            event_log.clear();
            object_log.clear();
            current_step = 0;
        }
    };
}