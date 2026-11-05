// glTF UV（TEXCOORD_0）解析单元测试
#include "test_framework.hpp"
#include "render/json.hpp"
#include "render/gltf_loader.hpp"
#include <fstream>
#include <cstring>

using namespace rvdlrsp;

static std::vector<uint8_t> make_glb(const std::string &json_str, const std::vector<uint8_t> &bin) {
    uint32_t magic = 0x46546C67, version = 2;
    std::vector<uint8_t> out;
    auto push32 = [&](uint32_t v) {
        uint8_t b[4]; std::memcpy(b, &v, 4); out.insert(out.end(), b, b + 4);
    };
    push32(magic); push32(version);
    size_t len_pos = out.size(); push32(0);

    uint32_t json_len = static_cast<uint32_t>(json_str.size());
    uint32_t json_padded = (json_len + 3) & ~3u;
    push32(json_padded); push32(0x4E4F534A);
    out.insert(out.end(), json_str.begin(), json_str.end());
    out.resize(out.size() + (json_padded - json_len), ' ');

    uint32_t bin_len = static_cast<uint32_t>(bin.size());
    uint32_t bin_padded = (bin_len + 3) & ~3u;
    push32(bin_padded); push32(0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    out.resize(out.size() + (bin_padded - bin_len), 0);

    uint32_t total = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + len_pos, &total, 4);
    return out;
}

static void pushf(std::vector<uint8_t> &bin, float v) {
    uint8_t b[4]; std::memcpy(b, &v, 4); bin.insert(bin.end(), b, b + 4);
}
static void pushu16(std::vector<uint8_t> &bin, uint16_t v) {
    uint8_t b[2]; std::memcpy(b, &v, 2); bin.insert(bin.end(), b, b + 2);
}

QTEST(gltf_uv_parse) {
    // 3 顶点，带 POSITION(3) + TEXCOORD_0(2)，3 索引
    // bufferViews: 0=POSITION(36B), 1=TEXCOORD(24B), 2=indices(6B)
    std::string json = R"({
        "buffers": [{"byteLength": 66}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 24},
            {"buffer": 0, "byteOffset": 60, "byteLength": 6}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2}]}]
    })";

    std::vector<uint8_t> bin;
    // POSITION
    pushf(bin, 0); pushf(bin, 0); pushf(bin, 0);
    pushf(bin, 1); pushf(bin, 0); pushf(bin, 0);
    pushf(bin, 0); pushf(bin, 1); pushf(bin, 0);
    // TEXCOORD
    pushf(bin, 0.0f); pushf(bin, 0.0f);
    pushf(bin, 1.0f); pushf(bin, 0.0f);
    pushf(bin, 0.5f); pushf(bin, 1.0f);
    // indices
    pushu16(bin, 0); pushu16(bin, 1); pushu16(bin, 2);

    std::vector<uint8_t> glb = make_glb(json, bin);
    std::ofstream f("_tmp_uv.glb", std::ios::binary);
    f.write(reinterpret_cast<char *>(glb.data()), glb.size());
    f.close();

    render::GltfResult r = render::GltfLoader::load_asset("_tmp_uv.glb");
    QCHECK(r.mesh.vertices.size() == 3);

    // 顶点 0：UV (0,0)
    QCHECK_NEAR(r.mesh.vertices[0].u, 0.0f, 1e-5);
    QCHECK_NEAR(r.mesh.vertices[0].v, 0.0f, 1e-5);
    // 顶点 1：UV (1,0)
    QCHECK_NEAR(r.mesh.vertices[1].u, 1.0f, 1e-5);
    QCHECK_NEAR(r.mesh.vertices[1].v, 0.0f, 1e-5);
    // 顶点 2：UV (0.5,1)
    QCHECK_NEAR(r.mesh.vertices[2].u, 0.5f, 1e-5);
    QCHECK_NEAR(r.mesh.vertices[2].v, 1.0f, 1e-5);

    std::remove("_tmp_uv.glb");
}

QTEST(gltf_no_uv_defaults) {
    // 无 TEXCOORD_0 时 UV 应为 0
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
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}]
    })";

    std::vector<uint8_t> bin;
    for (int i = 0; i < 9; ++i) pushf(bin, 0.0f);
    pushu16(bin, 0); pushu16(bin, 1); pushu16(bin, 2);

    std::vector<uint8_t> glb = make_glb(json, bin);
    std::ofstream f("_tmp_nouv.glb", std::ios::binary);
    f.write(reinterpret_cast<char *>(glb.data()), glb.size());
    f.close();

    render::GltfResult r = render::GltfLoader::load_asset("_tmp_nouv.glb");
    QCHECK(r.mesh.vertices.size() == 3);
    QCHECK_NEAR(r.mesh.vertices[0].u, 0.0f, 1e-5);
    QCHECK_NEAR(r.mesh.vertices[0].v, 0.0f, 1e-5);

    std::remove("_tmp_nouv.glb");
}
