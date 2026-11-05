#pragma once
#include <string>
#include <stdexcept>
#include <cctype>
#include "scene.hpp"
#include "obj_loader.hpp"
#include "gltf_loader.hpp"

namespace rvdlrsp::render {

    class MeshLoader {
    public:
        static Mesh load(const std::string &path) {
            // 提取扩展名
            size_t dot = path.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
            for (auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (ext == "obj") return ObjLoader::load(path);
            if (ext == "gltf" || ext == "glb") return GltfLoader::load(path);
            throw std::runtime_error("MeshLoader: unsupported format '." + ext + "'");
        }
    };
}