#pragma once
#include <string>
#include <vector>
#include <bitset>
#include <cmath>
#include <stdexcept>
#include "../include/qhal/IQuantumBackend.hpp"
#include "../include/numqk/Numqk.hpp"
#include "../include/qml/Inference.hpp" // next_available_qubit
#include "QObject.hpp"
#include "../../vendor/stb/stb_image.h" // stbi_load（图像解码，实现见 src/stb_image_impl.cpp）
namespace rvdl
{
    namespace qml
    {
        class RVDL_RT_API QDataEncoder
        {
        private:
            qhal::IQuantumBackend *backend;

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
                // 从全局 qubit id 空间分配（避免与 QuantumRegister 等对象冲突），复用已释放 id
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

            // 振幅编码（amplitude encoding）：文本 → 2^k 维特征向量 → 归一化 →
            // 态振幅。用 k = ⌈log2 N⌉ 个 qubit 表示 N 维特征，比逐字符 8-qubit
            // 编码（text_to_basis）省 qubit（"memory" 48 qubit → 6~8 qubit）。
            std::shared_ptr<QDataState> text_to_amplitudes(const std::string &text, size_t num_qubits)
            {
                const size_t N = size_t(1) << num_qubits;
                std::vector<std::complex<double>> amps(N, {0.0, 0.0});

                // 特征：字符（位置加权哈希）到 N 个振幅 bin（bag-of-chars 的量子化）
                size_t pos = 0;
                for (unsigned char c : text)
                {
                    const size_t bin = (static_cast<size_t>(c) * 2654435761ULL + pos * 97ULL) % N;
                    amps[bin] += std::complex<double>(1.0, 0.0);
                    ++pos;
                }

                // 归一化
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

                // 先分配全局 id 并扩展后端容量，再设置态（prepare 内部仅按需补建 dense/MPS）
                std::vector<size_t> qubits_ids(num_qubits);
                for (size_t i = 0; i < num_qubits; ++i)
                {
                    qubits_ids[i] = rvdl_alloc_qubit_id();
                    backend->allocate_qubit(qubits_ids[i]);
                }
                backend->prepare_amplitudes(amps, num_qubits);
                return std::make_shared<QDataState>(backend, qubits_ids);
            }

            // 自适应感知分配：按后端可表示的最大 qubit 数选择编码方式。
            //   • QVM：按可用内存/压缩策略（MPS）最大化；
            //   • 真实 QM：按芯片物理 qubit 数最大化。
            //   naive 逐字符（8 qubit/char）可容纳时用 naive（精确）；否则回退
            //   到振幅编码（log2 压缩）。手动分配仍可用 text_to_basis / text_to_amplitudes。
            std::shared_ptr<QDataState> encode_adaptive(const std::string &text)
            {
                size_t max_q = backend->get_max_qubits();
                if (max_q == 0)
                    max_q = 24; // 后端未知时保守上限

                const size_t naive_q = text.size() * 8;
                if (naive_q <= max_q)
                    return text_to_basis(text); // 精确的逐字符编码

                // 振幅编码：用 k = ⌈log2(naive_q)⌉ qubit 压缩，且不超过 max_q。
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

            // 图像振幅编码：加载图像文件（stb_image，支持 PNG/JPEG/BMP/PGM 等），
            // 强制灰度、展平后均匀下采样到 N = 2^num_qubits 个像素，
            // 像素强度归一化到 [0,1] 作为振幅，再 L2 归一化。
            // 返回占用 num_qubits 个 qubit 的振幅编码态。
            std::shared_ptr<QDataState> image_to_amplitudes(const std::string &path, size_t num_qubits)
            {
                if (num_qubits == 0 || num_qubits > 20)
                    throw std::invalid_argument("image_to_amplitudes: num_qubits out of range (1..20)");

                int w = 0, h = 0, channels = 0;
                // desired_channels=1 → 强制 8-bit 灰度
                unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 1);
                if (!pixels)
                    throw std::runtime_error("image_to_amplitudes: failed to load image '" + path + "'");

                const size_t N = size_t(1) << num_qubits;
                std::vector<std::complex<double>> amps(N, {0.0, 0.0});

                // 展平 + 均匀下采样：把 w×h 灰度图映射到 N 个振幅 bin（按面积平均）
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
                    amps[i] = std::complex<double>(avg / 255.0, 0.0); // 像素强度 → [0,1] 振幅
                }
                stbi_image_free(pixels);

                // L2 归一化（全黑图退化为 |0..0>）
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

                // 从全局 qubit id 空间分配 + 直接设置振幅（同 text_to_amplitudes）
                std::vector<size_t> qubits_ids(num_qubits);
                for (size_t i = 0; i < num_qubits; ++i)
                {
                    qubits_ids[i] = rvdl_alloc_qubit_id();
                    backend->allocate_qubit(qubits_ids[i]);
                }
                backend->prepare_amplitudes(amps, num_qubits);
                return std::make_shared<QDataState>(backend, qubits_ids);
            }
        };
    }
}