// glTF 材质（baseColorFactor / baseColorTexture）单元测试
#include "test_framework.hpp"
#include "render/json.hpp"
#include "render/gltf_loader.hpp"
#include <fstream>
#include <cstring>

using namespace rvdlrsp;
using namespace rvdlrsp::json;

// 构造 .glb 文件字节流
static std::vector<uint8_t> make_glb(const std::string &json_str, const std::vector<uint8_t> &bin) {
    uint32_t magic = 0x46546C67, version = 2;
    std::vector<uint8_t> out;
    auto push32 = [&](uint32_t v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    };
    push32(magic);
    push32(version);
    size_t len_pos = out.size();
    push32(0);

    uint32_t json_len = static_cast<uint32_t>(json_str.size());
    uint32_t json_padded = (json_len + 3) & ~3u;
    push32(json_padded);
    push32(0x4E4F534A);
    out.insert(out.end(), json_str.begin(), json_str.end());
    out.resize(out.size() + (json_padded - json_len), ' ');

    uint32_t bin_len = static_cast<uint32_t>(bin.size());
    uint32_t bin_padded = (bin_len + 3) & ~3u;
    push32(bin_padded);
    push32(0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    out.resize(out.size() + (bin_padded - bin_len), 0);

    uint32_t total = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + len_pos, &total, 4);
    return out;
}

QTEST(gltf_material_basecolor_factor) {
    // 一个三角形 + 一个材质（baseColorFactor 红色）
    std::string json = R"({
        "buffers": [{"byteLength": 42}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "materials": [{
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 0.0, 0.0, 1.0],
                "metallicFactor": 0.3,
                "roughnessFactor": 0.8
            }
        }],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}]
    })";

    std::vector<uint8_t> bin;
    auto pushf = [&](float v) { uint8_t b[4]; std::memcpy(b, &v, 4); bin.insert(bin.end(), b, b+4); };
    auto pushu16 = [&](uint16_t v) { uint8_t b[2]; std::memcpy(b, &v, 2); bin.insert(bin.end(), b, b+2); };
    pushf(0); pushf(0); pushf(0);
    pushf(1); pushf(0); pushf(0);
    pushf(0); pushf(1); pushf(0);
    pushu16(0); pushu16(1); pushu16(2);

    std::vector<uint8_t> glb = make_glb(json, bin);
    std::ofstream f("_tmp_mat.glb", std::ios::binary);
    f.write(reinterpret_cast<char *>(glb.data()), glb.size());
    f.close();

    render::GltfResult r = render::GltfLoader::load_asset("_tmp_mat.glb");
    QCHECK(r.materials.size() == 1);
    QCHECK_NEAR(r.materials[0].base_color[0], 1.0f, 1e-6);
    QCHECK_NEAR(r.materials[0].base_color[1], 0.0f, 1e-6);
    QCHECK_NEAR(r.materials[0].metallic, 0.3f, 1e-6);
    QCHECK_NEAR(r.materials[0].roughness, 0.8f, 1e-6);
    QCHECK(r.material_index == 0);

    std::remove("_tmp_mat.glb");
}

QTEST(gltf_material_texture) {
    // 材质 + baseColorTexture → texture → image（bufferView 引用）
    std::string json = R"({
        "buffers": [{"byteLength": 48}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6},
            {"buffer": 0, "byteOffset": 42, "byteLength": 4}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "images": [{"bufferView": 2, "mimeType": "image/png"}],
        "textures": [{"source": 0}],
        "materials": [{
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0}
            }
        }],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}]
    })";

    std::vector<uint8_t> bin;
    auto pushf = [&](float v) { uint8_t b[4]; std::memcpy(b, &v, 4); bin.insert(bin.end(), b, b+4); };
    auto pushu16 = [&](uint16_t v) { uint8_t b[2]; std::memcpy(b, &v, 2); bin.insert(bin.end(), b, b+2); };
    pushf(0); pushf(0); pushf(0);
    pushf(1); pushf(0); pushf(0);
    pushf(0); pushf(1); pushf(0);
    pushu16(0); pushu16(1); pushu16(2);
    // 图像数据（4 字节 PNG 占位）
    bin.push_back(0x89); bin.push_back('P'); bin.push_back('N'); bin.push_back('G');

    std::vector<uint8_t> glb = make_glb(json, bin);
    std::ofstream f("_tmp_tex.glb", std::ios::binary);
    f.write(reinterpret_cast<char *>(glb.data()), glb.size());
    f.close();

    render::GltfResult r = render::GltfLoader::load_asset("_tmp_tex.glb");
    QCHECK(r.materials.size() == 1);
    QCHECK(r.materials[0].base_color_texture.valid);
    QCHECK(r.materials[0].base_color_texture.mime == "image/png");
    QCHECK(r.materials[0].base_color_texture.data.size() == 4);

    std::remove("_tmp_tex.glb");
}
