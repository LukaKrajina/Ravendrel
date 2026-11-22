#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "qpc/math.hpp"

namespace rvdlrsp::render {

    struct Vertex {
        qpc::Vec3 position;
        qpc::Vec3 normal;
        float u = 0.0f, v = 0.0f;
        float r = 1.0f, g = 1.0f, b = 1.0f;
    };

    struct Mesh {
        std::string name;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    struct Camera {
        qpc::Vec3 position{0, 2, 10};
        qpc::Vec3 target{0, 0, 0};
        double fov_deg = 60.0;
        double near_plane = 0.1;
        double far_plane = 100.0;
    };

    struct Texture {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
        std::string mime;
        bool valid = false;
    };

    struct DecodedImage {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        bool valid = false;
    };

    struct Material {
        float base_color[3] = {0.8f, 0.8f, 0.8f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        Texture base_color_texture;
    };

    struct SceneInstance {
        uint32_t mesh_id = 0;
        uint32_t material_id = 0;
        qpc::Vec3 position;
        qpc::Quat orientation;
        qpc::Vec3 scale{1, 1, 1};
    };

    struct DirectionalLight {
        qpc::Vec3 direction{0.3, -1.0, 0.5};
        float intensity = 1.0f;
    };

    struct Scene {
        std::vector<Mesh> meshes;
        std::vector<Material> materials;
        std::vector<SceneInstance> instances;
        Camera camera;
        DirectionalLight light;
    };
}