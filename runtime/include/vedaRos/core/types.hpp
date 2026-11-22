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

    struct QuantumHeader
    {
        uint64_t timestamp_ns = 0;
        uint32_t frame_id = 0;
        uint32_t sequence = 0;
    };

    struct QubitState
    {
        std::complex<double> alpha{1.0, 0.0};
        std::complex<double> beta{0.0, 0.0};
    };

    struct AmplitudeVector
    {
        uint32_t num_qubits = 0;
        std::vector<std::complex<double>> amplitudes;
    };

    class Serializable
    {
    public:
        virtual ~Serializable() = default;
        virtual TypeId type_id() const = 0;
        virtual void serialize(std::ostringstream &os) const = 0;
        virtual bool deserialize(std::istringstream &is) = 0;
    };

    struct TypeSafePayload
    {
        TypeId type = TypeId::Void;
        std::vector<uint8_t> classical_bytes;
        AmplitudeVector quantum_state;
    };
}