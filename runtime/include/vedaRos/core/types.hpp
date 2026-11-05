#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <complex>
#include <sstream>

namespace vedaros
{

    enum class TypeId : uint16_t
    {
        Void = 0,
        Bool,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float,
        Double,
        String,
        Complex,
        QubitState,
        AmplitudeVector,
        Header,
        Custom = 0x8000
    };

    // 经典消息头（时间戳 + 帧 + 序号）
    struct QuantumHeader
    {
        uint64_t timestamp_ns = 0;
        uint32_t frame_id = 0;
        uint32_t sequence = 0;
    };

    // 单比特量子态（跨语言可序列化）
    struct QubitState
    {
        std::complex<double> alpha{1.0, 0.0};
        std::complex<double> beta{0.0, 0.0};
    };

    // 多比特态矢量
    struct AmplitudeVector
    {
        uint32_t num_qubits = 0;
        std::vector<std::complex<double>> amplitudes;
    };

    // 类型安全的序列化基类：写入时带类型 ID，读取时校验
    class Serializable
    {
    public:
        virtual ~Serializable() = default;
        virtual TypeId type_id() const = 0;
        virtual void serialize(std::ostringstream &os) const = 0;
        virtual bool deserialize(std::istringstream &is) = 0;
    };

    // 量子-经典双通道负载：经典走字节流，量子走态矢量
    struct TypeSafePayload
    {
        TypeId type = TypeId::Void;
        std::vector<uint8_t> classical_bytes;
        AmplitudeVector quantum_state;
    };
}