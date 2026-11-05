#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include <string>
#include <memory>
#include <stdexcept>
#include "../include/qhal/IQuantumBackend.hpp"
#include "../include/qml/Inference.hpp" // rvdl_alloc_qubit_id / rvdl_free_qubit_id（全局 qubit id 复用）

namespace rvdl
{
    class RVDL_RT_API QObject
    {
    protected:
        qhal::IQuantumBackend *backend;
        std::vector<size_t> hardware_ids;
        bool is_owning;

        QObject(qhal::IQuantumBackend *active_backend, const std::vector<size_t> &existing_ids, bool owns_resource)
            : backend(active_backend), hardware_ids(existing_ids), is_owning(owns_resource) {}

    public:
        void *qlm_data = nullptr;

        QObject(qhal::IQuantumBackend *active_backend, size_t num_qubits) : backend(active_backend), is_owning(true)
        {
            // 【qubit 回收优化】原实现用连续 id 0..n-1，多个 QuantumRegister 并发时
            // 会共享同一批 id（冲突），且空闲 id 无法复用（QVM num_qubits 只增不减）。
            // 改为走全局 id 分配器（复用已释放 id），使并发 QObject 各得唯一 id，
            // 峰值 qubit 数由「最大并发对象数」而非「累计创建对象数」决定。
            hardware_ids.reserve(num_qubits);
            for (size_t i = 0; i < num_qubits; ++i)
            {
                size_t id = rvdl_alloc_qubit_id();
                backend->allocate_qubit(id);
                hardware_ids.push_back(id);
            }
        }

        virtual ~QObject()
        {
            if (is_owning)
            {
                // release_qubit 内部已完成「测量坍缩 + 复位到 |0⟩ + 收缩 num_qubits」，
                // 此处无需再显式 measure/apply_x（避免重复复位）。
                // 额外把 id 回收到全局空闲栈，供后续对象复用。
                for (size_t id : hardware_ids)
                {
                    backend->release_qubit(id);
                    rvdl_free_qubit_id(id);
                }
            }
            else
            {
                for (size_t id : hardware_ids)
                {
                    backend->unlock_hardware_id(id);
                }
            }
        }

        virtual void reset_to_ground_state() = 0;

        virtual std::vector<int> measure()
        {
            std::vector<int> results;
            results.reserve(hardware_ids.size());
            for (size_t id : hardware_ids)
            {
                results.push_back(backend->measure(id));
            }
            return results;
        }

        virtual size_t size() const
        {
            return hardware_ids.size();
        }

        const std::vector<size_t> &get_ids() const
        {
            return hardware_ids;
        }
    };

    class RVDL_RT_API DiracState : public QObject
    {
    private:
        bool init_to_one;

        bool holds_superposition;

    public:
        DiracState(qhal::IQuantumBackend *active_backend, bool start_in_excited_state = false)
            : QObject(active_backend, 1), init_to_one(start_in_excited_state), holds_superposition(false)
        {
            reset_to_ground_state();
        }

        DiracState(qhal::IQuantumBackend *active_backend, size_t existing_id)
            : QObject(active_backend, std::vector<size_t>{existing_id}, false),
              init_to_one(false),
              holds_superposition(true)
        {
            backend->lock_hardware_id(existing_id);
        }

        void reset_to_ground_state() override
        {
            int current_state = backend->measure(hardware_ids[0]);

            if (current_state == 1)
            {
                backend->apply_x(hardware_ids[0]);
            }

            if (init_to_one)
            {
                backend->apply_x(hardware_ids[0]);
            }
            holds_superposition = false;
        }
    };

    class RVDL_RT_API BellState : public QObject
    {
    public:
        BellState(qhal::IQuantumBackend *backend)
            : QObject(backend, 2)
        {
            reset_to_ground_state();
        }

        void reset_to_ground_state() override
        {
            size_t q0 = hardware_ids[0];
            size_t q1 = hardware_ids[1];

            backend->apply_rz(q0, M_PI / 2);
            backend->apply_x(q0);
            backend->apply_rz(q0, M_PI / 2);
            backend->apply_cnot(q0, q1);
        }
    };

    class RVDL_RT_API QuantumRegister : public QObject
    {
    public:
        QuantumRegister(qhal::IQuantumBackend *backend, size_t n)
            : QObject(backend, n)
        {
            reset_to_ground_state();
        }

        void reset_to_ground_state() override
        {
            for (size_t id : hardware_ids)
            {
                int collapsed_state = backend->measure(id);
                if (collapsed_state == 1)
                {
                    backend->apply_x(id);
                }
            }
        }

        DiracState extract_qubit(size_t index)
        {
            if (index >= size())
                throw std::out_of_range("Ravendrel Runtime Error: Register index out of bounds.");

            return DiracState(backend, hardware_ids[index]);
        }
    };

    class RVDL_RT_API QDataState : public QObject
    {
    public:
        QDataState(qhal::IQuantumBackend *backend, const std::vector<size_t> &encoded_ids)
            : QObject(backend, encoded_ids, true) {}

        void reset_to_ground_state() override
        {
            for (size_t id : hardware_ids)
            {
                int collapsed_state = backend->measure(id);
                if (collapsed_state == 1)
                {
                    backend->apply_x(id);
                }
            }
        }
    };
}