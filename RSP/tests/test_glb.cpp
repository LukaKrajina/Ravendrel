#include "test_framework.hpp"
#include "render/json.hpp"
#include "render/gltf_loader.hpp"
#include <fstream>
#include <cstring>

using namespace rvdlrsp;
using namespace rvdlrsp::json;

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

QTEST(glb_parse_triangle) {
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
    auto pushf = [&](float v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4);
        bin.insert(bin.end(), b, b + 4);
    };
    auto pushu16 = [&](uint16_t v) {
        uint8_t b[2];
        std::memcpy(b, &v, 2);
        bin.insert(bin.end(), b, b + 2);
    };
    pushf(0); pushf(0); pushf(0);
    pushf(1); pushf(0); pushf(0);
    pushf(0); pushf(1); pushf(0);
    pushu16(0); pushu16(1); pushu16(2);

    std::vector<uint8_t> glb = make_glb(json, bin);

    std::ofstream f("_tmp_test.glb", std::ios::binary);
    f.write(reinterpret_cast<char *>(glb.data()), glb.size());
    f.close();

    render::Mesh mesh = render::GltfLoader::load("_tmp_test.glb");
    QCHECK(mesh.vertices.size() == 3);
    QCHECK(mesh.indices.size() == 3);
    QCHECK_NEAR(mesh.vertices[1].position.x, 1.0, 1e-6);
    QCHECK(mesh.indices[2] == 2);

    std::remove("_tmp_test.glb");
}

QTEST(glb_invalid_magic) {
    std::vector<uint8_t> bad(16, 0);
    std::ofstream f("_tmp_bad.glb", std::ios::binary);
    f.write(reinterpret_cast<char *>(bad.data()), bad.size());
    f.close();

    bool threw = false;
    try {
        render::GltfLoader::load("_tmp_bad.glb");
    } catch (const std::exception &) {
        threw = true;
    }
    QCHECK(threw);
    std::remove("_tmp_bad.glb");
}