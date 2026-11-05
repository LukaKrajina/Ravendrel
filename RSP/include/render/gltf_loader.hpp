#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <stdexcept>
#include "json.hpp"
#include "scene.hpp"

namespace rvdlrsp::render
{

    // glTF 加载结果：网格 + 材质 + 网格关联的材质索引
    struct GltfResult
    {
        Mesh mesh;
        std::vector<Material> materials;
        uint32_t material_index = 0;
    };

    class GltfLoader
    {
    public:
        // 统一入口：按扩展名分发 .gltf / .glb，返回网格
        static Mesh load(const std::string &path)
        {
            return load_asset(path).mesh;
        }

        // 加载：返回网格 + 材质
        static GltfResult load_asset(const std::string &path)
        {
            size_t dot = path.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
            for (auto &c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == "glb")
                return load_glb(path);
            return load_gltf(path);
        }

    private:
        // ─── .gltf────────────────
        static GltfResult load_gltf(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw std::runtime_error("glTF: cannot open " + path);
            std::string json_str((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            json::Value root = json::parse(json_str);

            std::vector<std::vector<uint8_t>> buffers;
            const json::Value &buffers_v = root.at("buffers");
            for (const auto &buf : buffers_v.array())
                buffers.push_back(load_buffer(path, buf));

            return parse_asset(root, buffers, path);
        }

        // ─── .glb────────────────────────────
        static GltfResult load_glb(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw std::runtime_error("glb: cannot open " + path);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());

            if (bytes.size() < 12)
                throw std::runtime_error("glb: file too small");

            uint32_t magic;
            std::memcpy(&magic, bytes.data(), 4);
            if (magic != 0x46546C67)
                throw std::runtime_error("glb: invalid magic");

            uint32_t version;
            std::memcpy(&version, bytes.data() + 4, 4);
            if (version != 2)
                throw std::runtime_error("glb: unsupported version (need 2)");

            std::string json_str;
            std::vector<uint8_t> bin_chunk;
            size_t off = 12;
            while (off + 8 <= bytes.size())
            {
                uint32_t chunk_len, chunk_type;
                std::memcpy(&chunk_len, bytes.data() + off, 4);
                std::memcpy(&chunk_type, bytes.data() + off + 4, 4);
                off += 8;
                if (off + chunk_len > bytes.size())
                    throw std::runtime_error("glb: chunk exceeds file");

                if (chunk_type == 0x4E4F534A)
                {
                    json_str.assign(reinterpret_cast<char *>(bytes.data() + off), chunk_len);
                }
                else if (chunk_type == 0x004E4942)
                {
                    bin_chunk.assign(bytes.begin() + off, bytes.begin() + off + chunk_len);
                }
                off += chunk_len;
            }

            json::Value root = json::parse(json_str);

            std::vector<std::vector<uint8_t>> buffers;
            const json::Value &buffers_v = root.at("buffers");
            for (size_t i = 0; i < buffers_v.array().size(); ++i)
            {
                const json::Value &buf = buffers_v[i];
                if (i == 0 && !bin_chunk.empty())
                {
                    buffers.push_back(bin_chunk);
                }
                else
                {
                    buffers.push_back(load_buffer(path, buf));
                }
            }

            return parse_asset(root, buffers, path);
        }

        // ─── 共享：解析 mesh + 材质 ────────────────────────
        static GltfResult parse_asset(const json::Value &root,
                                      const std::vector<std::vector<uint8_t>> &buffers,
                                      const std::string &name)
        {
            GltfResult result;

            const json::Value &meshes = root.at("meshes");
            const json::Value &prim = meshes[0].at("primitives")[0];

            Mesh &mesh = result.mesh;
            mesh.name = name;

            std::vector<double> positions = read_accessor(root, buffers,
                                                          prim.at("attributes").at("POSITION").number(), 3);

            std::vector<double> normals;
            bool has_normals = prim.at("attributes").object().count("NORMAL") > 0;
            if (has_normals)
                normals = read_accessor(root, buffers,
                                        prim.at("attributes").at("NORMAL").number(), 3);

            // TEXCOORD_0（可选，vec2）
            std::vector<double> uvs;
            bool has_uvs = false;
            if (prim.at("attributes").object().count("TEXCOORD_0") > 0)
            {
                uvs = read_accessor(root, buffers,
                                    prim.at("attributes").at("TEXCOORD_0").number(), 2);
                has_uvs = !uvs.empty();
            }

            size_t vertex_count = positions.size() / 3;
            for (size_t i = 0; i < vertex_count; ++i)
            {
                Vertex v;
                v.position = {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
                if (has_normals)
                    v.normal = {normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
                if (has_uvs && i * 2 + 1 < uvs.size())
                {
                    v.u = static_cast<float>(uvs[i * 2]);
                    v.v = static_cast<float>(uvs[i * 2 + 1]);
                }
                v.r = 0.6f;
                v.g = 0.7f;
                v.b = 0.8f;
                mesh.vertices.push_back(v);
            }

            if (prim.object().count("indices") > 0)
            {
                std::vector<double> indices = read_accessor(root, buffers,
                                                            prim.at("indices").number(), 1);
                for (double idx : indices)
                    mesh.indices.push_back(static_cast<uint32_t>(idx));
            }

            // primitive 关联的材质索引
            if (prim.object().count("material") > 0)
                result.material_index = static_cast<uint32_t>(prim.at("material").number());

            // 解析所有材质
            if (root.object().count("materials") > 0)
                parse_materials(root, buffers, result.materials);

            return result;
        }

        // ─── 解析 PBR 材质 ────────────────────────────────
        static void parse_materials(const json::Value &root,
                                    const std::vector<std::vector<uint8_t>> &buffers,
                                    std::vector<Material> &out)
        {
            for (const auto &mat_v : root.at("materials").array())
            {
                Material m;
                if (mat_v.object().count("pbrMetallicRoughness") > 0)
                {
                    const json::Value &pbr = mat_v.at("pbrMetallicRoughness");
                    if (pbr.object().count("baseColorFactor") > 0)
                    {
                        const json::Value &bcf = pbr.at("baseColorFactor");
                        m.base_color[0] = static_cast<float>(bcf[0].number());
                        m.base_color[1] = static_cast<float>(bcf[1].number());
                        m.base_color[2] = static_cast<float>(bcf[2].number());
                    }
                    if (pbr.object().count("metallicFactor") > 0)
                        m.metallic = static_cast<float>(pbr.at("metallicFactor").number());
                    if (pbr.object().count("roughnessFactor") > 0)
                        m.roughness = static_cast<float>(pbr.at("roughnessFactor").number());

                    // baseColorTexture → texture → image
                    if (pbr.object().count("baseColorTexture") > 0)
                    {
                        int tex_idx = static_cast<int>(
                            pbr.at("baseColorTexture").at("index").number());
                        load_texture(root, buffers, tex_idx, m.base_color_texture);
                    }
                }
                out.push_back(m);
            }
        }

        // ─── 解析纹理（image → bufferView 或 uri）───────────
        static void load_texture(const json::Value &root,
                                 const std::vector<std::vector<uint8_t>> &buffers,
                                 int tex_idx, Texture &out)
        {
            if (root.object().count("textures") == 0)
                return;
            const json::Value &tex = root.at("textures")[static_cast<size_t>(tex_idx)];
            int img_idx = static_cast<int>(tex.at("source").number());
            const json::Value &img = root.at("images")[static_cast<size_t>(img_idx)];

            if (img.object().count("mimeType") > 0)
                out.mime = img.at("mimeType").string();

            // bufferView 引用（内嵌图像数据）
            if (img.object().count("bufferView") > 0)
            {
                int bv_idx = static_cast<int>(img.at("bufferView").number());
                const json::Value &bv = root.at("bufferViews")[static_cast<size_t>(bv_idx)];
                int buf_idx = static_cast<int>(bv.at("buffer").number());
                size_t bv_offset = static_cast<size_t>(bv.at("byteOffset").number());
                size_t bv_len = static_cast<size_t>(bv.at("byteLength").number());
                const std::vector<uint8_t> &buf = buffers[static_cast<size_t>(buf_idx)];
                out.data.assign(buf.begin() + bv_offset, buf.begin() + bv_offset + bv_len);
                out.valid = true;
                return;
            }

            // uri 引用（data URI 或外部文件）
            if (img.object().count("uri") > 0)
            {
                std::string uri = img.at("uri").string();
                if (uri.rfind("data:", 0) == 0)
                {
                    size_t comma = uri.find(',');
                    if (comma != std::string::npos)
                    {
                        out.data = base64_decode(uri.substr(comma + 1));
                        out.valid = true;
                    }
                }
            }
        }

        // 读取 buffer（data URI base64 或外部 .bin）
        static std::vector<uint8_t> load_buffer(const std::string &gltf_path, const json::Value &buf)
        {
            const std::string &uri = buf.at("uri").string();
            if (uri.rfind("data:", 0) == 0)
            {
                size_t comma = uri.find(',');
                if (comma == std::string::npos)
                    throw std::runtime_error("glTF: invalid data URI");
                return base64_decode(uri.substr(comma + 1));
            }
            size_t slash = gltf_path.find_last_of("/\\");
            std::string dir = (slash == std::string::npos) ? "" : gltf_path.substr(0, slash + 1);
            std::ifstream f(dir + uri, std::ios::binary);
            if (!f)
                throw std::runtime_error("glTF: cannot open buffer " + uri);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
        }

        // 读取 accessor
        static std::vector<double> read_accessor(const json::Value &root,
                                                 const std::vector<std::vector<uint8_t>> &buffers,
                                                 int accessor_idx, int components)
        {
            const json::Value &acc = root.at("accessors")[static_cast<size_t>(accessor_idx)];
            int buffer_view = static_cast<int>(acc.at("bufferView").number());
            int count = static_cast<int>(acc.at("count").number());
            size_t offset = acc.object().count("byteOffset") > 0
                                ? static_cast<size_t>(acc.at("byteOffset").number())
                                : 0;

            const json::Value &bv = root.at("bufferViews")[static_cast<size_t>(buffer_view)];
            int buf_idx = static_cast<int>(bv.at("buffer").number());
            size_t bv_offset = static_cast<size_t>(bv.at("byteOffset").number());

            const std::vector<uint8_t> &buffer = buffers[static_cast<size_t>(buf_idx)];
            std::vector<double> out;
            out.reserve(static_cast<size_t>(count) * components);

            int comp_type = static_cast<int>(acc.at("componentType").number());
            size_t total_offset = bv_offset + offset;

            for (int i = 0; i < count * components; ++i)
            {
                if (comp_type == 5126)
                {
                    float val;
                    std::memcpy(&val, buffer.data() + total_offset + i * 4, 4);
                    out.push_back(static_cast<double>(val));
                }
                else if (comp_type == 5123)
                {
                    uint16_t val;
                    std::memcpy(&val, buffer.data() + total_offset + i * 2, 2);
                    out.push_back(static_cast<double>(val));
                }
                else
                {
                    throw std::runtime_error("glTF: unsupported componentType");
                }
            }
            return out;
        }

        static std::vector<uint8_t> base64_decode(const std::string &in)
        {
            static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::vector<uint8_t> out;
            int val = 0, bits = -8;
            for (char c : in)
            {
                if (c == '=')
                    break;
                const char *p = strchr(table, c);
                if (!p)
                    continue;
                val = (val << 6) + static_cast<int>(p - table);
                bits += 6;
                if (bits >= 0)
                {
                    out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
                    bits -= 8;
                }
            }
            return out;
        }
    };
}