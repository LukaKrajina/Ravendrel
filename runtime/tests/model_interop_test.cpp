#include "qml/ModelInterop.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace qml::interop;
using qml::interop::u32;
using qml::interop::u64;
using qml::interop::u8;

namespace
{
    int g_fail = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) ++g_fail;
    }

    std::string g_out_dir = "model_interop_out";


    std::vector<u8> read_all(const std::string &p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string fnv_hex(const std::vector<u8> &b)
    {
        u64 h = 0xCBF29CE484222325ull;
        for (u8 x : b)
        {
            h ^= x;
            h *= 0x100000001B3ull;
        }
        char buf[24];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
        return buf;
    }

    std::string json_escape(const std::string &s)
    {
        std::string o = "\"";
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back((char)c); }
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o.push_back((char)c);
        }
        o += "\"";
        return o;
    }

    void write_expected_json(const std::string &path, const TensorData &t, const Metadata &meta)
    {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        o << "{\n";
        o << "  \"name\": " << json_escape(t.name) << ",\n";
        o << "  \"dtype\": \"" << dtype_name(t.dtype) << "\",\n";
        o << "  \"shape\": [";
        for (size_t i = 0; i < t.shape.size(); ++i) o << (i ? ", " : "") << t.shape[i];
        o << "],\n";
        o << "  \"bytes\": " << t.bytes.size() << ",\n";
        o << "  \"fnv1a64\": \"" << fnv_hex(t.bytes) << "\",\n";
        o << "  \"metadata\": {";
        for (size_t i = 0; i < meta.size(); ++i)
            o << (i ? ", " : "") << json_escape(meta[i].first) << ": " << json_escape(meta[i].second);
        o << "}\n}\n";
    }


    struct SafeSt
    {
        u64 header_len = 0;
        size_t json_at = 8;
        size_t data_at = 0;
        size_t data_len = 0;
        std::string json;
        bool padded = false;
        std::string name, dtype;
        std::vector<u64> shape;
        u64 off_begin = 0, off_end = 0;
        std::string err;
    };

    bool safe_parse(const std::vector<u8> &f, const std::string &tensor_name, SafeSt &st)
    {
        if (f.size() < 8) { st.err = "太短"; return false; }
        for (int i = 0; i < 8; ++i) st.header_len |= (u64)f[i] << (8 * i);
        if (8 + st.header_len > f.size()) { st.err = "头长度越界"; return false; }
        st.padded = ((8 + st.header_len) % 8) == 0;
        st.json.assign((const char *)f.data() + st.json_at, (size_t)st.header_len);
        st.data_at = 8 + (size_t)st.header_len;
        st.data_len = f.size() - st.data_at;

        const std::string key = "\"" + tensor_name + "\"";
        size_t at = st.json.find(key);
        if (at == std::string::npos) { st.err = "找不到张量名"; return false; }
        st.name = tensor_name;

        auto find_after = [&](const char *k, size_t from) { return st.json.find(k, from); };
        size_t d = find_after("\"dtype\":", at);
        if (d == std::string::npos) { st.err = "没有 dtype"; return false; }
        size_t q1 = st.json.find('"', d + 8), q2 = st.json.find('"', q1 + 1);
        st.dtype = st.json.substr(q1 + 1, q2 - q1 - 1);

        size_t sh = find_after("\"shape\":[", at);
        if (sh == std::string::npos) { st.err = "没有 shape"; return false; }
        size_t close = st.json.find(']', sh);
        std::string dims = st.json.substr(sh + 9, close - sh - 9);
        st.shape.clear();
        for (size_t i = 0; i < dims.size();)
        {
            while (i < dims.size() && (dims[i] == ' ' || dims[i] == ',')) ++i;
            u64 v = 0;
            bool any = false;
            while (i < dims.size() && dims[i] >= '0' && dims[i] <= '9') { v = v * 10 + (u64)(dims[i] - '0'); ++i; any = true; }
            if (any) st.shape.push_back(v);
        }

        size_t of = find_after("\"data_offsets\":[", at);
        if (of == std::string::npos) { st.err = "没有 data_offsets"; return false; }
        size_t oc = st.json.find(']', of);
        std::string offs = st.json.substr(of + 16, oc - of - 16);
        std::sscanf(offs.c_str(), " %llu , %llu", (unsigned long long *)&st.off_begin,
                    (unsigned long long *)&st.off_end);
        return true;
    }


    struct GgufSt
    {
        u32 version = 0;
        u64 n_tensors = 0, n_kv = 0;
        std::map<std::string, std::string> kv;
        std::string name;
        u32 n_dims = 0;
        std::vector<u64> ne;
        u32 gtype = 0;
        u64 off = 0;
        size_t data_at = 0;
        size_t data_end = 0;
        std::string err;
    };

    struct BR
    {
        const u8 *p;
        size_t n, at = 0;
        bool need(size_t k) { return at + k <= n; }
        u8 b1() { return p[at++]; }
        u32 u32le() { u32 v = 0; for (int i = 0; i < 4; ++i) v |= (u32)p[at + i] << (8 * i); at += 4; return v; }
        u64 u64le() { u64 v = 0; for (int i = 0; i < 8; ++i) v |= (u64)p[at + i] << (8 * i); at += 8; return v; }
        std::string str() { u64 k = u64le(); std::string s((const char *)p + at, (size_t)k); at += (size_t)k; return s; }
    };

    bool gguf_parse(const std::vector<u8> &f, GgufSt &st)
    {
        if (f.size() < 24 || std::memcmp(f.data(), "GGUF", 4) != 0) { st.err = "魔数不对"; return false; }
        BR r{f.data(), f.size()};
        r.at = 4;
        st.version = r.u32le();
        st.n_tensors = r.u64le();
        st.n_kv = r.u64le();
        u32 alignment = 32;
        for (u64 i = 0; i < st.n_kv; ++i)
        {
            std::string k = r.str();
            u32 t = r.u32le();
            if (t == 8) { st.kv[k] = r.str(); }
            else if (t == 4) { u32 v = r.u32le(); st.kv[k] = std::to_string(v); if (k == "general.alignment") alignment = v; }
            else if (t == 10) { st.kv[k] = std::to_string(r.u64le()); }
            else if (t == 0 || t == 1 || t == 7) { st.kv[k] = std::to_string((int)r.b1()); }
            else { st.err = "KV 类型 " + std::to_string(t) + " 不在最小读取器支持范围内"; return false; }
        }
        st.name = r.str();
        st.n_dims = r.u32le();
        st.ne.resize(st.n_dims);
        for (u32 i = 0; i < st.n_dims; ++i) st.ne[i] = r.u64le();
        st.gtype = r.u32le();
        st.off = r.u64le();
        st.data_at = (r.at + alignment - 1) / alignment * alignment;
        st.data_end = f.size();
        return true;
    }


    TensorData make_f32()
    {
        TensorData t;
        t.name = "weight";
        t.dtype = DType::F32;
        t.shape = {2, 3};
        const float vals[6] = {0.5f, -1.25f, 3.0f, 0.0f, -0.0f, 1e-7f};
        t.bytes.resize(24);
        std::memcpy(t.bytes.data(), vals, 24);
        return t;
    }
}

int main()
{
    if (const char *e = std::getenv("MODEL_INTEROP_OUT")) g_out_dir = e;
    std::filesystem::create_directories(g_out_dir);

    std::printf("[1] safetensors：布局、对齐、数据段\n");
    {
        const TensorData t = make_f32();
        Metadata meta{{"format", "rvdlm"}, {"name", "toy"}, {"说明", "中文元数据"}, {"quote", "a\"b\\c"}};
        std::string err;
        const std::string path = g_out_dir + "/toy.safetensors";
        check(save_safetensors(path, t, meta, &err), "写出成功" + (err.empty() ? "" : "（" + err + "）"));

        const std::vector<u8> f = read_all(path);
        SafeSt st;
        if (!safe_parse(f, "weight", st))
            check(false, "独立解析：" + st.err);
        else
        {
            check(st.padded, "头长度补齐到 8 字节");
            check(st.dtype == "F32", "dtype = F32（独立读到 " + st.dtype + "）");
            check(st.shape == std::vector<u64>({2, 3}), "shape = [2,3]");
            check(st.off_end - st.off_begin == 24, "data_offsets 长度 = 24 字节");
            check(st.off_end <= st.data_len, "data_offsets 落在数据段内");
            std::vector<u8> got(f.begin() + (std::ptrdiff_t)(st.data_at + st.off_begin),
                                f.begin() + (std::ptrdiff_t)(st.data_at + st.off_end));
            check(got == t.bytes, "数据段逐字节等于原始权重");
        }

        std::vector<TensorData> ts;
        Metadata m2;
        check(load_safetensors(path, ts, m2, &err) && ts.size() == 1, "load_safetensors 读回");
        check(!ts.empty() && ts[0].bytes == t.bytes && ts[0].shape == t.shape, "读回的数据与形状一致");
        check(m2 == meta, "读回的元数据与原值一致（含中文与引号）");

        write_expected_json(g_out_dir + "/expected_safetensors.json", t, meta);
    }

    std::printf("[2] gguf：维度逆序、general.alignment、数据段\n");
    {
        const TensorData t = make_f32();
        Metadata meta{{"name", "toy"}, {"architecture", "rvdlm"}, {"note", "gguf 往返"}};
        std::string err;
        const std::string path = g_out_dir + "/toy.gguf";
        check(save_gguf(path, t, meta, &err), "写出成功" + (err.empty() ? "" : "（" + err + "）"));

        const std::vector<u8> f = read_all(path);
        GgufSt st;
        if (!gguf_parse(f, st))
            check(false, "独立解析：" + st.err);
        else
        {
            check(st.version == 3, "版本 = 3");
            check(st.n_tensors == 1, "张量数 = 1");
            check(st.kv.count("general.alignment") && st.kv["general.alignment"] == "32",
                  "general.alignment = 32");
            check(st.name == "weight", "张量名 = weight");
            check(st.n_dims == 2, "维度数 = 2");
            check(st.ne == std::vector<u64>({3, 2}), "维度逆序：行主序 [2,3] 写成 ne = [3,2]");
            check(st.gtype == 0, "ggml 类型 = 0（F32）");
            check(st.off == 0, "单张量偏移 = 0");
            check(st.data_at % 32 == 0, "数据段起点 32 字节对齐");
            check(st.data_at + 24 <= st.data_end, "数据段长度足够");
            std::vector<u8> got(f.begin() + (std::ptrdiff_t)(st.data_at),
                                f.begin() + (std::ptrdiff_t)(st.data_at + 24));
            check(got == t.bytes, "数据段逐字节等于原始权重");
        }

        std::vector<TensorData> ts;
        Metadata m2;
        check(load_gguf(path, ts, m2, &err) && ts.size() == 1, "load_gguf 读回");
        check(!ts.empty() && ts[0].shape == t.shape, "读回的维度已转回行主序（[2,3]）");
        check(!ts.empty() && ts[0].bytes == t.bytes, "读回的数据一致");

        write_expected_json(g_out_dir + "/expected_gguf.json", t, meta);
    }

    std::printf("[3] 类型映射：宁缺毋猜\n");
    {
        std::string err;
        TensorData u8t;
        u8t.name = "flags";
        u8t.dtype = DType::U8;
        u8t.shape = {4};
        u8t.bytes = {1, 2, 3, 4};
        check(!save_gguf(g_out_dir + "/nope.gguf", u8t, {}, &err), "u8 → gguf 必须失败");
        check(err.find("GGUF") != std::string::npos, "失败信息指名 GGUF 没有该类型");
        check(!read_all(g_out_dir + "/nope.gguf").size(), "失败时不留下半个文件");

        check(save_safetensors(g_out_dir + "/flags.safetensors", u8t, {}, &err), "u8 → safetensors 可以");
        std::vector<TensorData> ts;
        Metadata m;
        check(load_safetensors(g_out_dir + "/flags.safetensors", ts, m, &err) && ts.size() == 1 &&
                  ts[0].dtype == DType::U8,
              "U8 往返类型不变");

        TensorData bt;
        bt.name = "mask";
        bt.dtype = DType::BOOL;
        bt.shape = {2};
        bt.bytes = {1, 0};
        check(!save_gguf(g_out_dir + "/nope2.gguf", bt, {}, &err), "bool → gguf 必须失败（ggml 无 bool）");

        TensorData f64t;
        f64t.name = "pi";
        f64t.dtype = DType::F64;
        f64t.shape = {2};
        double dvals[2] = {3.14159265358979, -2.5};
        f64t.bytes.resize(16);
        std::memcpy(f64t.bytes.data(), dvals, 16);
        check(save_gguf(g_out_dir + "/pi.gguf", f64t, {}, &err), "f64 → gguf 可以（ggml type 28）");
        check(load_gguf(g_out_dir + "/pi.gguf", ts, m, &err) && ts.size() == 1 && ts[0].dtype == DType::F64 &&
                  ts[0].bytes == f64t.bytes,
              "f64 往返一致");
    }

    std::printf("[4] 写出必须是确定的\n");
    {
        const TensorData t = make_f32();
        std::unordered_map<std::string, std::string> um;
        um["name"] = "toy";
        um["zeta"] = "1";
        um["alpha"] = "2";
        um["说明"] = "中文";
        Metadata meta = sorted_metadata(um);
        std::string e1, e2;
        save_safetensors(g_out_dir + "/det1.safetensors", t, meta, &e1);
        save_safetensors(g_out_dir + "/det2.safetensors", t, meta, &e2);
        check(read_all(g_out_dir + "/det1.safetensors") == read_all(g_out_dir + "/det2.safetensors"),
              "两次导出的 safetensors 逐字节相同");
        check(meta.front().first == "alpha" && meta[1].first == "name" && meta[2].first == "zeta" &&
                  meta.back().first == "说明",
              "元数据按 key 排序写出（UTF-8 字节序，中文在 ASCII 之后）");
    }

    std::printf("[5] `.rvdlm` → 两种容器（端到端）\n");
    {
        numqk::Tensor<float> q({2, 3}, false);
        for (size_t i = 0; i < q.size(); ++i) q.data()[i] = (float)i * 0.25f - 0.5f;
        std::unordered_map<std::string, std::string> meta{{"name", "toy"}, {"step", "7"}};
        qml::RVDLModel<float> model(q, meta);
        const std::string rv = g_out_dir + "/toy.rvdlm";
        qml::ModelExporter<float>::save(rv, model);

        std::string err;
        check(export_rvdlm_to_safetensors<float>(rv, g_out_dir + "/toy_e2e.safetensors", "weight", &err),
              "rvdlm → safetensors" + (err.empty() ? "" : "（" + err + "）"));
        check(export_rvdlm_to_gguf<float>(rv, g_out_dir + "/toy_e2e.gguf", "weight", &err),
              "rvdlm → gguf" + (err.empty() ? "" : "（" + err + "）"));

        std::vector<TensorData> ts;
        Metadata m;
        if (load_safetensors(g_out_dir + "/toy_e2e.safetensors", ts, m, &err) && ts.size() == 1)
        {
            bool same = ts[0].shape == std::vector<u64>({2, 3});
            for (size_t i = 0; i < q.size() && same; ++i)
            {
                float got;
                std::memcpy(&got, ts[0].bytes.data() + i * 4, 4);
                same = got == q.data()[i];
            }
            check(same, "safetensors 里的数值与 .rvdlm 张量逐位相同");
            check(m == sorted_metadata(meta), "元数据随 .rvdlm 一起过去（有序）");
        }
        else
            check(false, "端到端 safetensors 读回失败：" + err);
    }

    std::printf("[6] 边界拒绝：越界 / 长度不符 / 对齐 / 版本\n");
    {
        std::string err;
        {
            std::string js = "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,9999]}}";
            while ((8 + js.size()) % 8) js.push_back(' ');
            std::vector<u8> f;
            for (int i = 0; i < 8; ++i) f.push_back((u8)(((u64)js.size() >> (8 * i)) & 0xFF));
            f.insert(f.end(), js.begin(), js.end());
            for (int i = 0; i < 8; ++i) f.push_back(0);
            std::ofstream o(g_out_dir + "/bad_off.safetensors", std::ios::binary);
            o.write((const char *)f.data(), (std::streamsize)f.size());
        }
        std::vector<TensorData> ts;
        Metadata m;
        check(!load_safetensors(g_out_dir + "/bad_off.safetensors", ts, m, &err), "越界 offsets 被拒绝");
        check(err.find("越界") != std::string::npos, "错误信息说明了原因（" + err + "）");

        {
            std::string js = "{\"w\":{\"dtype\":\"BF16\",\"shape\":[2],\"data_offsets\":[0,4]}}";
            while ((8 + js.size()) % 8) js.push_back(' ');
            std::vector<u8> f;
            for (int i = 0; i < 8; ++i) f.push_back((u8)(((u64)js.size() >> (8 * i)) & 0xFF));
            f.insert(f.end(), js.begin(), js.end());
            for (int i = 0; i < 4; ++i) f.push_back(0);
            std::ofstream o(g_out_dir + "/bad_dtype.safetensors", std::ios::binary);
            o.write((const char *)f.data(), (std::streamsize)f.size());
        }
        check(!load_safetensors(g_out_dir + "/bad_dtype.safetensors", ts, m, &err), "BF16 被拒绝（不猜）");

        {
            std::vector<u8> f;
            auto u32p = [&](u32 v) { for (int i = 0; i < 4; ++i) f.push_back((u8)((v >> (8 * i)) & 0xFF)); };
            auto u64p = [&](u64 v) { for (int i = 0; i < 8; ++i) f.push_back((u8)((v >> (8 * i)) & 0xFF)); };
            auto strp = [&](const std::string &s) { u64p(s.size()); f.insert(f.end(), s.begin(), s.end()); };
            f.insert(f.end(), {'G', 'G', 'U', 'F'});
            u32p(3);
            u64p(1);
            u64p(1);
            strp("general.alignment");
            u32p(4);
            u32p(24);
            strp("w");
            u32p(1);
            u64p(2);
            u32p(0);
            u64p(0);
            std::ofstream o(g_out_dir + "/bad_align.gguf", std::ios::binary);
            o.write((const char *)f.data(), (std::streamsize)f.size());
        }
        check(!load_gguf(g_out_dir + "/bad_align.gguf", ts, m, &err), "alignment = 24 被拒绝");
        check(err.find("2 的幂") != std::string::npos, "错误信息指出对齐不是 2 的幂");

        {
            std::vector<u8> f = read_all(g_out_dir + "/toy.gguf");
            if (f.size() > 8) f[4] = 2;
            std::ofstream o(g_out_dir + "/bad_v2.gguf", std::ios::binary);
            o.write((const char *)f.data(), (std::streamsize)f.size());
        }
        check(!load_gguf(g_out_dir + "/bad_v2.gguf", ts, m, &err), "GGUF v2 被拒绝（维度宽度不同）");
    }

    std::printf("[7] 容器 → `.rvdlm`（反向导入 + 对账）\n");
    {
        numqk::Tensor<float> q({3, 2}, false);
        for (size_t i = 0; i < q.size(); ++i) q.data()[i] = (float)i * 0.5f - 1.0f;
        std::unordered_map<std::string, std::string> meta{{"name", "roundtrip"}, {"step", "42"}};
        qml::RVDLModel<float> model(q, meta);
        const std::string rv0 = g_out_dir + "/rt_src.rvdlm";
        qml::ModelExporter<float>::save(rv0, model);

        std::string err;
        const std::string st = g_out_dir + "/rt.safetensors";
        const std::string gf = g_out_dir + "/rt.gguf";
        check(export_rvdlm_to_safetensors<float>(rv0, st, "weight", &err),
              "round-trip: rvdlm → safetensors" + (err.empty() ? "" : "（" + err + "）"));
        check(export_rvdlm_to_gguf<float>(rv0, gf, "weight", &err),
              "round-trip: rvdlm → gguf" + (err.empty() ? "" : "（" + err + "）"));

        for (const std::string &container : {st, gf})
        {
            const std::string back = container + ".rvdlm";
            const bool is_gguf = container == gf;
            const bool ok = is_gguf
                ? import_gguf_to_rvdlm_auto(container, back, "weight", &err)
                : import_safetensors_to_rvdlm_auto(container, back, "weight", &err);
            check(ok, (is_gguf ? "gguf" : "safetensors") + std::string(" → .rvdlm")
                          + (err.empty() ? "" : "（" + err + "）"));
            if (!ok) continue;
            try
            {
                qml::RVDLModel<float> rt = qml::ModelExporter<float>::load(back);
                bool same = rt.tensor.get_shape() == q.get_shape() && rt.tensor.size() == q.size();
                for (size_t i = 0; i < q.size() && same; ++i)
                    same = rt.tensor.data()[i] == q.data()[i];
                check(same, std::string(is_gguf ? "gguf" : "safetensors") + " round-trip 张量逐位相同");
                bool no_loss = true, no_general = true;
                for (const auto &kv : meta)
                    if (rt.metadata.find(kv.first) == rt.metadata.end() ||
                        rt.metadata.at(kv.first) != kv.second) no_loss = false;
                for (const auto &kv : rt.metadata)
                    if (kv.first.rfind("general.", 0) == 0) no_general = false;
                check(no_loss, std::string(is_gguf ? "gguf" : "safetensors") + " round-trip 元数据无丢失");
                check(no_general, std::string(is_gguf ? "gguf" : "safetensors")
                              + " 容器级 general.* 键不泄漏进 .rvdlm");
                if (is_gguf)
                    check(rt.metadata.size() == meta.size() + 1 &&
                              rt.metadata.count("architecture") == 1,
                          "gguf round-trip 只多出补默认值的 architecture（单调超集，不丢键）");
                else
                    check(rt.metadata == meta, "safetensors round-trip 元数据键值相等（逐字往返）");
            }
            catch (const std::exception &e)
            {
                check(false, std::string("round-trip 读回失败：") + e.what());
            }
        }

        {
            numqk::Tensor<double> dq({2}, false);
            dq.data()[0] = 0.125; dq.data()[1] = -3.5;
            std::unordered_map<std::string, std::string> dmeta{{"k", "v"}};
            qml::RVDLModel<double> dmodel(dq, dmeta);
            const std::string rv = g_out_dir + "/f64.rvdlm";
            qml::ModelExporter<double>::save(rv, dmodel);
            check(export_rvdlm_to_safetensors<double>(rv, g_out_dir + "/f64.safetensors", "weight", &err),
                  "f64 rvdlm → safetensors");
            const std::string back = g_out_dir + "/f64_back.rvdlm";
            check(import_safetensors_to_rvdlm_auto(g_out_dir + "/f64.safetensors", back, "weight", &err),
                  "auto 导入按容器 dtype（F64）分发" + (err.empty() ? "" : "（" + err + "）"));
            try
            {
                qml::RVDLModel<double> rt = qml::ModelExporter<double>::load(back);
                check(rt.tensor.data()[0] == 0.125 && rt.tensor.data()[1] == -3.5,
                      "F64 round-trip 数值精确（未被按 F32 重组）");
            }
            catch (const std::exception &e) { check(false, std::string("F64 读回失败：") + e.what()); }
        }

        check(!import_safetensors_to_rvdlm<double>(st, g_out_dir + "/mismatch.rvdlm", "weight", &err),
              "容器是 F32 却按 double 导入 → 拒绝");
        check(err.find("类型不一致") != std::string::npos,
              "拒绝信息说明类型不一致（" + err + "）");

        check(!import_safetensors_to_rvdlm_auto(st, g_out_dir + "/noname.rvdlm", "nope", &err),
              "不存在的张量名 → 拒绝");
        check(err.find("可用") != std::string::npos, "拒绝信息列出可用张量名（" + err + "）");

        {
            TensorData a; a.name = "alpha"; a.dtype = DType::F32; a.shape = {2};
            a.bytes.resize(8);
            float va[2] = {1.f, 2.f};
            std::memcpy(a.bytes.data(), va, 8);
            TensorData b = a; b.name = "beta"; b.shape = {3}; b.bytes.resize(12);
            float vb[3] = {7.f, 8.f, 9.f};
            std::memcpy(b.bytes.data(), vb, 12);
            const std::string multi = g_out_dir + "/multi.safetensors";
            std::string js = "{";
            for (size_t i = 0; i < 2; ++i)
            {
                const TensorData &t = (i == 0 ? a : b);
                if (i) js += ",";
                js += "\"" + t.name + "\":{\"dtype\":\"F32\",\"shape\":[";
                for (size_t k = 0; k < t.shape.size(); ++k) js += (k ? "," : "") + std::to_string(t.shape[k]);
                js += "],\"data_offsets\":[" + std::to_string(i == 0 ? 0 : 8) + ","
                      + std::to_string((i == 0 ? 0 : 8) + t.bytes.size()) + "]}";
            }
            js += "}";
            while ((8 + js.size()) % 8) js.push_back(' ');
            std::vector<u8> f;
            for (int i = 0; i < 8; ++i) f.push_back((u8)(((u64)js.size() >> (8 * i)) & 0xFF));
            f.insert(f.end(), js.begin(), js.end());
            f.insert(f.end(), a.bytes.begin(), a.bytes.end());
            f.insert(f.end(), b.bytes.begin(), b.bytes.end());
            {
                std::ofstream o(multi, std::ios::binary);
                o.write((const char *)f.data(), (std::streamsize)f.size());
            }
            std::vector<TensorData> ts;
            Metadata m;
            check(load_safetensors(multi, ts, m, &err) && ts.size() == 2,
                  "多张量 safetensors 读回 2 个张量");
            check(import_safetensors_to_rvdlm_auto(multi, g_out_dir + "/multi_first.rvdlm", "", &err),
                  "缺省张量名 → 取第一个");
            check(import_safetensors_to_rvdlm_auto(multi, g_out_dir + "/multi_beta.rvdlm", "beta", &err),
                  "指名 beta → 取 beta");
            try
            {
                qml::RVDLModel<float> rt = qml::ModelExporter<float>::load(g_out_dir + "/multi_beta.rvdlm");
                check(rt.tensor.size() == 3 && rt.tensor.data()[0] == 7.f && rt.tensor.data()[2] == 9.f,
                      "beta 的数值确实被导入（不是 alpha）");
            }
            catch (const std::exception &e) { check(false, std::string("beta 读回失败：") + e.what()); }
        }

        {
            std::vector<TensorData> ts;
            Metadata m;
            std::vector<u8> f;
            auto u32p = [&](u32 v) { for (int i = 0; i < 4; ++i) f.push_back((u8)((v >> (8 * i)) & 0xFF)); };
            auto u64p = [&](u64 v) { for (int i = 0; i < 8; ++i) f.push_back((u8)((v >> (8 * i)) & 0xFF)); };
            auto strp = [&](const std::string &s) { u64p(s.size()); f.insert(f.end(), s.begin(), s.end()); };
            f.insert(f.end(), {'G', 'G', 'U', 'F'});
            u32p(3);
            u64p(1);
            u64p(1);
            strp("general.alignment");
            u32p(4);
            u32p(32);
            strp("w");
            u32p(1);
            u64p(2);
            u32p(1);
            u64p(0);
            {
                std::ofstream o(g_out_dir + "/bad_gtype.gguf", std::ios::binary);
                o.write((const char *)f.data(), (std::streamsize)f.size());
            }
            check(!load_gguf(g_out_dir + "/bad_gtype.gguf", ts, m, &err),
                  "未知 ggml 类型被拒绝（量化类型不假装支持）");
            check(!import_gguf_to_rvdlm_auto(g_out_dir + "/bad_gtype.gguf", g_out_dir + "/x.rvdlm", "", &err),
                  "导入侧同样拒绝（不会把量化块当普通张量写进 .rvdlm）");
        }
    }

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "MODEL-INTEROP: 全部通过" : "MODEL-INTEROP: 失败", g_fail);
    std::printf("产物目录: %s\n", g_out_dir.c_str());
    return g_fail == 0 ? 0 : 1;
}