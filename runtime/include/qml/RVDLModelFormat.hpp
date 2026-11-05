#pragma once
#include "../numqk/Numqk.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cstdint>

namespace qml
{
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
            std::ofstream out(filepath, std::ios::binary);
            if (!out)
            {
                throw std::runtime_error("Hardware Error: Failed to open file for .rvdlm export");
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
                uint32_t k_len = kv.first.length();
                out.write(reinterpret_cast<const char *>(&k_len), sizeof(uint32_t));
                out.write(kv.first.c_str(), k_len);

                uint32_t v_len = kv.second.length();
                out.write(reinterpret_cast<const char *>(&v_len), sizeof(uint32_t));
                out.write(kv.second.c_str(), v_len);
            }

            out.write(reinterpret_cast<const char *>(model.tensor.data()), model.tensor.size() * sizeof(T));

            out.close();
            std::cout << "[QKM Exporter] Saved model to " << filepath
                      << " with " << header.metadata_count << " metadata keys.\n";
        }

        static RVDLModel<T> load(const std::string &filepath)
        {
            std::ifstream in(filepath, std::ios::binary);
            if (!in)
                throw std::runtime_error("Hardware Error: Failed to open .rvdlm file");

            QKMHeader header;
            in.read(reinterpret_cast<char *>(&header), sizeof(QKMHeader));

            if (header.magic[0] != 'Q' || header.magic[1] != 'K' ||
                header.magic[2] != 'M' || header.magic[3] != '1')
            {
                throw std::runtime_error("Format Error: Invalid .rvdlm magic number");
            }

            std::vector<size_t> loaded_shape;
            for (uint32_t i = 0; i < header.shape_dimensions; ++i)
            {
                uint64_t d;
                in.read(reinterpret_cast<char *>(&d), sizeof(uint64_t));
                loaded_shape.push_back(static_cast<size_t>(d));
            }

            std::unordered_map<std::string, std::string> loaded_metadata;
            for (uint32_t i = 0; i < header.metadata_count; ++i)
            {
                uint32_t k_len, v_len;

                in.read(reinterpret_cast<char *>(&k_len), sizeof(uint32_t));
                std::string key(k_len, '\0');
                in.read(&key[0], k_len);

                in.read(reinterpret_cast<char *>(&v_len), sizeof(uint32_t));
                std::string val(v_len, '\0');
                in.read(&val[0], v_len);

                loaded_metadata[key] = val;
            }

            numqk::Tensor<T> loaded_tensor(loaded_shape, false);
            in.read(reinterpret_cast<char *>(loaded_tensor.data()), header.total_elements * sizeof(T));

            in.close();
            return RVDLModel<T>(loaded_tensor, loaded_metadata);
        }
    };
}