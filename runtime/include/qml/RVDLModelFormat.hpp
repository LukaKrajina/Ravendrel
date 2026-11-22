#pragma once
#include "../numqk/Numqk.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <limits>
#include <atomic>
#include <chrono>
#include <filesystem>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace qml
{
    template <typename U> inline const char *dtype_tag() { return "unknown"; }
    template <> inline const char *dtype_tag<double>() { return "f64"; }
    template <> inline const char *dtype_tag<float>() { return "f32"; }
    template <> inline const char *dtype_tag<int32_t>() { return "i32"; }
    template <> inline const char *dtype_tag<int64_t>() { return "i64"; }
    template <> inline const char *dtype_tag<uint8_t>() { return "u8"; }

    struct QKMHeader
    {
        char magic[4] = {'Q', 'K', 'M', '1'};
        uint32_t version = 2;
        uint64_t total_elements = 0;
        uint32_t shape_dimensions = 0;
        uint32_t metadata_count = 0;
    };

    template <typename T>
    struct RVDLModel
    {
        numqk::Tensor<T> tensor;
        std::unordered_map<std::string, std::string> metadata;
        RVDLModel(numqk::Tensor<T> t) : tensor(t) {}
        RVDLModel(numqk::Tensor<T> t, std::unordered_map<std::string, std::string> meta)
            : tensor(t), metadata(meta) {}
    };

    template <typename T>
    class ModelExporter
    {
    public:
        static void save(const std::string &filepath, const RVDLModel<T> &model)
        {
            write_file(filepath, model, true);
        }

        static void save_atomic(const std::string &filepath, const RVDLModel<T> &model)
        {
            namespace fs = std::filesystem;
            static std::atomic<uint64_t> counter{0};
            const uint64_t stamp = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            const std::string tmp =
                filepath + ".tmp." + std::to_string(stamp) + "." +
                std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

            try
            {
                write_file(tmp, model, false);
            }
            catch (...)
            {
                std::error_code ec;
                fs::remove(tmp, ec);
                throw;
            }

            std::error_code ec;
#if defined(_WIN32)
            if (!MoveFileExA(tmp.c_str(), filepath.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const DWORD win_err = GetLastError();
                fs::remove(tmp, ec);
                throw std::runtime_error(
                    "Hardware Error: cannot atomically replace '" + filepath +
                    "' with '" + tmp + "' (Win32 error " + std::to_string(win_err) + ")");
            }
#else
            fs::rename(tmp, filepath, ec);
            if (ec)
            {
                fs::remove(tmp, ec);
                throw std::runtime_error(
                    "Hardware Error: cannot atomically replace '" + filepath +
                    "' with '" + tmp + "': " + ec.message());
            }
#endif
            std::cout << "[QKM Exporter] Saved model to " << filepath
                      << " with " << model.metadata.size() << " metadata keys, "
                      << model.tensor.size() << " elements.\n";
        }

    private:
        static void write_file(const std::string &filepath, const RVDLModel<T> &model, bool log)
        {
            std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                throw std::runtime_error(
                    "Hardware Error: cannot open '" + filepath +
                    "' for .rvdlm export (permission denied, path missing, or file locked): " +
                    std::strerror(errno));
            }

            QKMHeader header;
            header.total_elements = model.tensor.size();
            header.shape_dimensions = model.tensor.get_shape().size();
            header.metadata_count = model.metadata.size();
            out.write(reinterpret_cast<const char *>(&header), sizeof(QKMHeader));

            for (size_t dim : model.tensor.get_shape())
            {
                uint64_t d = dim;
                out.write(reinterpret_cast<const char *>(&d), sizeof(uint64_t));
            }

            for (const auto &kv : model.metadata)
            {
                if (kv.first.size() > std::numeric_limits<uint32_t>::max() ||
                    kv.second.size() > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Format Error: metadata entry too large for .rvdlm");
                uint32_t k_len = static_cast<uint32_t>(kv.first.length());
                out.write(reinterpret_cast<const char *>(&k_len), sizeof(uint32_t));
                out.write(kv.first.c_str(), k_len);

                uint32_t v_len = static_cast<uint32_t>(kv.second.length());
                out.write(reinterpret_cast<const char *>(&v_len), sizeof(uint32_t));
                out.write(kv.second.c_str(), v_len);
            }

            out.write(reinterpret_cast<const char *>(model.tensor.data()),
                      static_cast<std::streamsize>(model.tensor.size() * sizeof(T)));

            if (!out)
                throw std::runtime_error(
                    "Hardware Error: write failed for '" + filepath +
                    "' (disk full or device removed): " + std::strerror(errno));

            out.close();
            if (!out)
                throw std::runtime_error("Hardware Error: close failed for '" + filepath + "'");

            if (log)
                std::cout << "[QKM Exporter] Saved model to " << filepath
                          << " with " << header.metadata_count << " metadata keys, "
                          << header.total_elements << " elements.\n";
        }

    public:
        static RVDLModel<T> load(const std::string &filepath)
        {
            std::ifstream in(filepath, std::ios::binary | std::ios::ate);
            if (!in)
                throw std::runtime_error(
                    "Hardware Error: cannot open .rvdlm '" + filepath +
                    "' (check path/permissions): " + std::strerror(errno));

            const std::streamoff file_size = in.tellg();
            if (file_size < static_cast<std::streamoff>(sizeof(QKMHeader)))
                throw std::runtime_error(
                    "Format Error: .rvdlm '" + filepath + "' is too small (" +
                    std::to_string(static_cast<long long>(file_size)) + " bytes)");
            in.seekg(0, std::ios::beg);

            auto read_exact = [&](void *dst, std::size_t bytes, const char *what) {
                in.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(bytes));
                if (in.gcount() != static_cast<std::streamsize>(bytes))
                    throw std::runtime_error(
                        "Format Error: truncated .rvdlm '" + filepath + "' while reading " +
                        what + " (wanted " + std::to_string(bytes) + " bytes, got " +
                        std::to_string(static_cast<long long>(in.gcount())) + ")");
            };

            QKMHeader header;
            read_exact(&header, sizeof(QKMHeader), "header");

            if (header.magic[0] != 'Q' || header.magic[1] != 'K' ||
                header.magic[2] != 'M' || header.magic[3] != '1')
            {
                throw std::runtime_error(
                    "Format Error: invalid .rvdlm magic in '" + filepath +
                    "' (not a QKM1 model, or the file is corrupted)");
            }

            if (header.shape_dimensions > 64)
                throw std::runtime_error(
                    "Format Error: .rvdlm '" + filepath + "' has an unreasonable rank (" +
                    std::to_string(header.shape_dimensions) + ")");

            std::vector<size_t> loaded_shape;
            uint64_t expected_elements = 1;
            for (uint32_t i = 0; i < header.shape_dimensions; ++i)
            {
                uint64_t d;
                read_exact(&d, sizeof(uint64_t), "shape");
                if (d == 0)
                    throw std::runtime_error("Format Error: zero-sized dimension in '" + filepath + "'");
                loaded_shape.push_back(static_cast<size_t>(d));
                if (expected_elements > std::numeric_limits<uint64_t>::max() / d)
                    throw std::runtime_error("Format Error: shape overflow in '" + filepath + "'");
                expected_elements *= d;
            }
            if (expected_elements != header.total_elements)
                throw std::runtime_error(
                    "Format Error: .rvdlm '" + filepath + "' shape product (" +
                    std::to_string(expected_elements) + ") != header total_elements (" +
                    std::to_string(header.total_elements) + ")");

            if (header.metadata_count > 100000)
                throw std::runtime_error(
                    "Format Error: .rvdlm '" + filepath + "' has an unreasonable metadata count");

            std::unordered_map<std::string, std::string> loaded_metadata;
            for (uint32_t i = 0; i < header.metadata_count; ++i)
            {
                uint32_t k_len, v_len;

                read_exact(&k_len, sizeof(uint32_t), "metadata key length");
                if (k_len > (1u << 20))
                    throw std::runtime_error("Format Error: metadata key too large in '" + filepath + "'");
                std::string key(k_len, '\0');
                if (k_len)
                    read_exact(&key[0], k_len, "metadata key");

                read_exact(&v_len, sizeof(uint32_t), "metadata value length");
                if (v_len > (1u << 20))
                    throw std::runtime_error("Format Error: metadata value too large in '" + filepath + "'");
                std::string val(v_len, '\0');
                if (v_len)
                    read_exact(&val[0], v_len, "metadata value");

                loaded_metadata[key] = val;
            }

            const auto dtype_it = loaded_metadata.find("model.dtype");
            if (dtype_it != loaded_metadata.end() && !dtype_it->second.empty())
            {
                const std::string expected = dtype_tag<T>();
                if (dtype_it->second != expected)
                    throw std::runtime_error(
                        "Format Error: .rvdlm '" + filepath + "' was exported as " +
                        dtype_it->second + " but is being loaded as " + expected +
                        " (silent weight corruption if ignored)");
            }

            if (header.total_elements > (1ULL << 32))
                throw std::runtime_error(
                    "Format Error: .rvdlm '" + filepath + "' claims " +
                    std::to_string(header.total_elements) + " elements (too large)");

            numqk::Tensor<T> loaded_tensor(loaded_shape, false);
            read_exact(loaded_tensor.data(),
                       static_cast<std::size_t>(header.total_elements) * sizeof(T),
                       "tensor payload");

            in.close();
            return RVDLModel<T>(loaded_tensor, loaded_metadata);
        }
    };
}