#pragma once
#include <string>
#include <vector>
#include <bitset>
#include <cmath>
#include <stdexcept>
#include "../include/qhal/IQuantumBackend.hpp"
#include "../include/numqk/Numqk.hpp"
#include "../include/qml/Inference.hpp"
#include "QObject.hpp"
#include "../../vendor/stb/stb_image.h"
namespace rvdl
{
    namespace qml
    {
        class RVDL_RT_API QDataEncoder
        {
        private:
            qhal::IQuantumBackend *backend;

            static std::string decode_base64(const std::string &b64)
            {
                static const std::string alphabet =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string out;
                out.reserve(b64.size() * 3 / 4 + 3);
                unsigned val = 0;
                int bits = 0;
                for (char c : b64)
                {
                    if (c == '=') break;
                    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
                    const size_t idx = alphabet.find(c);
                    if (idx == std::string::npos)
                        throw std::runtime_error("image_ref_to_amplitudes: invalid base64 payload");
                    val = (val << 6) | static_cast<unsigned>(idx);
                    bits += 6;
                    if (bits >= 8)
                    {
                        bits -= 8;
                        out.push_back(static_cast<char>((val >> bits) & 0xFFu));
                        val &= (1u << bits) - 1u;
                    }
                }
                return out;
            }

            std::shared_ptr<QDataState> pixels_to_amplitudes(const unsigned char *pixels,
                                                            int w, int h, size_t num_qubits)
            {
                if (!pixels || w <= 0 || h <= 0 || num_qubits == 0 || num_qubits > 20)
                    throw std::invalid_argument("pixels_to_amplitudes: invalid input");

                const size_t N = size_t(1) << num_qubits;
                std::vector<std::complex<double>> amps(N, {0.0, 0.0});

                const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
                for (size_t i = 0; i < N; ++i)
                {
                    const size_t start = (i * total) / N;
                    size_t end = ((i + 1) * total) / N;
                    if (end <= start)
                        end = start + 1;
                    double acc = 0.0;
                    for (size_t p = start; p < end; ++p)
                        acc += static_cast<double>(pixels[p]);
                    const double avg = acc / static_cast<double>(end - start);
                    amps[i] = std::complex<double>(avg / 255.0, 0.0);
                }

                double norm = 0.0;
                for (const auto &a : amps)
                    norm += std::norm(a);
                if (norm > 1e-15)
                {
                    const double inv = 1.0 / std::sqrt(norm);
                    for (auto &a : amps)
                        a *= inv;
                }
                else
                {
                    amps[0] = std::complex<double>(1.0, 0.0);
                }

                std::vector<size_t> qubits_ids(num_qubits);
                for (size_t i = 0; i < num_qubits; ++i)
                {
                    qubits_ids[i] = rvdl_alloc_qubit_id();
                    backend->allocate_qubit(qubits_ids[i]);
                }
                backend->prepare_amplitudes(amps, num_qubits);
                return std::make_shared<QDataState>(backend, qubits_ids);
            }

        public:
            QDataEncoder(qhal::IQuantumBackend *target_backend) : backend(target_backend) {}

            std::shared_ptr<QDataState> text_to_basis(const std::string &text)
            {
                std::vector<bool> bit_stream;

                for (char c : text)
                {
                    std::bitset<8> bits(c);
                    for (int i = 7; i >= 0; --i)
                    {
                        bit_stream.push_back(bits[i]);
                    }
                }

                size_t num_qubits = bit_stream.size();
                std::vector<size_t> qubits_ids(num_qubits);
                for (size_t i = 0; i < num_qubits; ++i)
                {
                    qubits_ids[i] = rvdl_alloc_qubit_id();
                    backend->allocate_qubit(qubits_ids[i]);
                }

                for (size_t i = 0; i < num_qubits; ++i)
                {
                    if (bit_stream[i])
                    {
                        backend->apply_x(qubits_ids[i]);
                    }
                }

                return std::make_shared<QDataState>(backend, qubits_ids);
            }

            std::shared_ptr<QDataState> text_to_amplitudes(const std::string &text, size_t num_qubits)
            {
                const size_t N = size_t(1) << num_qubits;
                std::vector<std::complex<double>> amps(N, {0.0, 0.0});

                size_t pos = 0;
                for (unsigned char c : text)
                {
                    const size_t bin = (static_cast<size_t>(c) * 2654435761ULL + pos * 97ULL) % N;
                    amps[bin] += std::complex<double>(1.0, 0.0);
                    ++pos;
                }

                double norm = 0.0;
                for (const auto &a : amps)
                    norm += std::norm(a);
                if (norm > 1e-15)
                {
                    const double inv = 1.0 / std::sqrt(norm);
                    for (auto &a : amps)
                        a *= inv;
                }
                else
                {
                    amps[0] = std::complex<double>(1.0, 0.0);
                }

                std::vector<size_t> qubits_ids(num_qubits);
                for (size_t i = 0; i < num_qubits; ++i)
                {
                    qubits_ids[i] = rvdl_alloc_qubit_id();
                    backend->allocate_qubit(qubits_ids[i]);
                }
                backend->prepare_amplitudes(amps, num_qubits);
                return std::make_shared<QDataState>(backend, qubits_ids);
            }

            std::shared_ptr<QDataState> encode_adaptive(const std::string &text)
            {
                size_t max_q = backend->get_max_qubits();
                if (max_q == 0)
                    max_q = 24;

                const size_t naive_q = text.size() * 8;
                if (naive_q <= max_q)
                    return text_to_basis(text);

                size_t k = 1;
                while ((size_t(1) << k) < naive_q && k < max_q)
                    ++k;
                if (k > 30)
                    k = 30;
                return text_to_amplitudes(text, k);
            }

            std::shared_ptr<QDataState> image_to_angles(const numqk::Tensor<double> &normalized_image)
            {
                size_t num_qubits = normalized_image.size();
                backend->allocate_qubits(num_qubits);
                std::vector<size_t> qubit_ids(num_qubits);
                const double *pixels = normalized_image.data();

                for (size_t i = 0; i < num_qubits; ++i)
                {
                    qubit_ids[i] = i;

                    backend->apply_rz(i, M_PI / 2.0);
                    backend->apply_x(i);
                    backend->apply_rz(i, M_PI / 2.0);
                    double rotation_angle = pixels[i] * M_PI;
                    backend->apply_rz(i, rotation_angle);
                }
                
                return std::make_shared<QDataState>(backend, qubit_ids);
            }

            std::shared_ptr<QDataState> image_to_amplitudes(const std::string &path, size_t num_qubits)
            {
                if (num_qubits == 0 || num_qubits > 20)
                    throw std::invalid_argument("image_to_amplitudes: num_qubits out of range (1..20)");

                int w = 0, h = 0, channels = 0;
                unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 1);
                if (!pixels)
                    throw std::runtime_error("image_to_amplitudes: failed to load image '" + path + "'");

                auto state = pixels_to_amplitudes(pixels, w, h, num_qubits);
                stbi_image_free(pixels);
                return state;
            }

            std::shared_ptr<QDataState> image_ref_to_amplitudes(const std::string &ref, size_t num_qubits)
            {
                if (num_qubits == 0 || num_qubits > 20)
                    throw std::invalid_argument("image_ref_to_amplitudes: num_qubits out of range (1..20)");

                int w = 0, h = 0, channels = 0;
                unsigned char *pixels = nullptr;

                if (ref.rfind("data:", 0) == 0)
                {
                    const size_t comma = ref.find(',');
                    if (comma == std::string::npos)
                        throw std::runtime_error("image_ref_to_amplitudes: malformed data URL (no comma)");
                    const std::string meta = ref.substr(0, comma);
                    if (meta.find(";base64") == std::string::npos)
                        throw std::runtime_error("image_ref_to_amplitudes: data URL must be base64-encoded");

                    const std::string bytes = decode_base64(ref.substr(comma + 1));
                    if (bytes.empty())
                        throw std::runtime_error("image_ref_to_amplitudes: empty data URL payload");
                    pixels = stbi_load_from_memory(
                        reinterpret_cast<const stbi_uc *>(bytes.data()),
                        static_cast<int>(bytes.size()), &w, &h, &channels, 1);
                }
                else
                {
                    pixels = stbi_load(ref.c_str(), &w, &h, &channels, 1);
                }

                if (!pixels)
                    throw std::runtime_error("image_ref_to_amplitudes: failed to decode image reference");

                auto state = pixels_to_amplitudes(pixels, w, h, num_qubits);
                stbi_image_free(pixels);
                return state;
            }
        };
    }
}