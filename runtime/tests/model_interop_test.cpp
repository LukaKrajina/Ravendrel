// ============================================================
// model_interop_test.cpp —— `.rvdlm` → safetensors / gguf
//
// 这个测试里有两份**独立实现**，这是它存在的全部理由：
//
//   1. 生产代码（`qml/ModelInterop.hpp`）—— 写容器，也（可选地）读回来；
//   2. 本文件里的最小读取器 —— **不调用生产代码一行**，按规范直接解字节；
//   3. `model_interop_crosscheck.py` —— 第三种语言、第三份实现，用 Python 的
//      `struct` + `json` 再解一遍。
//
// 自己写、自己读，只能证明"自洽"：把 `ne[0]` 写反、把 offsets 写成相对文件起点、
// 把 JSON 长度算错 8 字节，这些错误都能在"自己读自己"的闭环里原样通过。
// 只有第三方按规范解字节才能发现。三种实现都同意，才叫符合规范。
//
// 另外还要证明两件**行为**上的事：
//   - 写出的文件必须逐字节确定（元数据顺序不能随 unordered_map 的迭代顺序漂移）；
//   - 无法无损映射的类型必须**报错而不是猜**（u8/bool → GGUF 没有对应 ggml 类型）。
//
// 运行：
//   clang++ -std=c++20 -O2 -I runtime/include runtime/tests/model_interop_test.cpp \
//       -o model_interop_test && ./model_interop_test
//   python3 runtime/tests/model_interop_crosscheck.py <测试输出的目录>
// ============================================================

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

    // ── 产物落盘（供 Python 交叉验证）────────────────────────────────

    std::vector<u8> read_all(const std::string &p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    /// FNV-1a 64：给 Python 侧一个"数据段一致"的判据（不用引第三方哈希库）。
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

    // ── 独立读取器 #1：safetensors（不碰生产代码）──────────────────────

    struct SafeSt
    {
        u64 header_len = 0;
        size_t json_at = 8;
        size_t data_at = 0;
        size_t data_len = 0;
        std::string json;
        bool padded = false;   ///< (8 + header_len) % 8 == 0
        std::string name, dtype;
        std::vector<u64> shape;
        u64 off_begin = 0, off_end = 0;
        std::string err;
    };

    /// 只认我们写出的那种最小 JSON（单张量 / 张量名 / 可选 __metadata__）。
    bool safe_parse(const std::vector<u8> &f, const std::string &tensor_name, SafeSt &st)
    {
        if (f.size() < 8) { st.err = "太短"; return false; }
        for (int i = 0; i < 8; ++i) st.header_len |= (u64)f[i] << (8 * i);
        if (8 + st.header_len > f.size()) { st.err = "头长度越界"; return false; }
        st.padded = ((8 + st.header_len) % 8) == 0;
        st.json.assign((const char *)f.data() + st.json_at, (size_t)st.header_len);
        st.data_at = 8 + (size_t)st.header_len;
        st.data_len = f.size() - st.data_at;

        // 手写字符串扫描：找 "tensor_name" 之后的 dtype / shape / data_offsets。
        // 故意**不**用生产代码的 JSON 解析器 —— 那份也是被检查的对象之一。
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

    // ── 独立读取器 #2：GGUF v3（不碰生产代码）─────────────────────────

    struct GgufSt
    {
        u32 version = 0;
        u64 n_tensors = 0, n_kv = 0;
        std::map<std::string, std::string> kv;
        std::string name;
        u32 n_dims = 0;
        std::vector<u64> ne;      ///< 文件里的原始顺序（ne[0] 最内层）
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

    // ── 造数据 ───────────────────────────────────────────────────────

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
} // namespace

int main()
{
    if (const char *e = std::getenv("MODEL_INTEROP_OUT")) g_out_dir = e;
    // 产物目录自建：否则从干净构建目录运行时第一节就静默写不出文件
    // （ofstream 打不开不抛异常，失败会延生长成莫名其妙的断言）。
    std::filesystem::create_directories(g_out_dir);

    // ── [1] safetensors：写出 + 独立解析 ────────────────────────────
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

        // 生产代码自己的读路径
        std::vector<TensorData> ts;
        Metadata m2;
        check(load_safetensors(path, ts, m2, &err) && ts.size() == 1, "load_safetensors 读回");
        check(!ts.empty() && ts[0].bytes == t.bytes && ts[0].shape == t.shape, "读回的数据与形状一致");
        check(m2 == meta, "读回的元数据与原值一致（含中文与引号）");

        write_expected_json(g_out_dir + "/expected_safetensors.json", t, meta);
    }

    // ── [2] gguf：维度逆序、对齐、数据段 ────────────────────────────
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
            // 【关键】ne[0] 是最内层：行主序 [2,3] ⇒ 文件里是 [3,2]
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

    // ── [3] 类型映射：能映射的要能往返，不能的必须报错 ──────────────
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

        // f64 / i32 / i64 → gguf 往返
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

    // ── [4] 确定性：同一输入两次导出逐字节相同 ──────────────────────
    std::printf("[4] 写出必须是确定的\n");
    {
        const TensorData t = make_f32();
        std::unordered_map<std::string, std::string> um;   // 故意用无序容器
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
        // 元数据必须已排序（否则 unordered_map 的迭代顺序会泄漏进文件）
        check(meta.front().first == "alpha" && meta[1].first == "name" && meta[2].first == "zeta" &&
                  meta.back().first == "说明",
              "元数据按 key 排序写出（UTF-8 字节序，中文在 ASCII 之后）");
    }

    // ── [5] `.rvdlm` 端到端 ─────────────────────────────────────────
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

    // ── [6] 坏文件必须在读取时就被拒绝 ──────────────────────────────
    std::printf("[6] 边界拒绝：越界 / 长度不符 / 对齐 / 版本\n");
    {
        std::string err;
        // 6a：手写一个 data_offsets 越界的 safetensors
        {
            std::string js = "{\"w\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,9999]}}";
            while ((8 + js.size()) % 8) js.push_back(' ');
            std::vector<u8> f;
            for (int i = 0; i < 8; ++i) f.push_back((u8)(((u64)js.size() >> (8 * i)) & 0xFF));
            f.insert(f.end(), js.begin(), js.end());
            for (int i = 0; i < 8; ++i) f.push_back(0);   // 数据段只有 8 字节（形状要 8）
            std::ofstream o(g_out_dir + "/bad_off.safetensors", std::ios::binary);
            o.write((const char *)f.data(), (std::streamsize)f.size());
        }
        std::vector<TensorData> ts;
        Metadata m;
        check(!load_safetensors(g_out_dir + "/bad_off.safetensors", ts, m, &err), "越界 offsets 被拒绝");
        check(err.find("越界") != std::string::npos, "错误信息说明了原因（" + err + "）");

        // 6b：手写一个 dtype 不认识的 safetensors（BF16）
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

        // 6c：手写一个 alignment = 24（不是 2 的幂）的 gguf
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
            u32p(4);   // UINT32
            u32p(24);  // 不是 2 的幂
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

        // 6d：v2 文件
        {
            std::vector<u8> f = read_all(g_out_dir + "/toy.gguf");
            if (f.size() > 8) f[4] = 2;
            std::ofstream o(g_out_dir + "/bad_v2.gguf", std::ios::binary);
            o.write((const char *)f.data(), (std::streamsize)f.size());
        }
        check(!load_gguf(g_out_dir + "/bad_v2.gguf", ts, m, &err), "GGUF v2 被拒绝（维度宽度不同）");
    }

    // ── [7] 反向导入：safetensors / gguf → `.rvdlm` ─────────────────
    std::printf("[7] 容器 → `.rvdlm`（反向导入 + 对账）\n");
    {
        // 7a：端到端 round-trip —— `.rvdlm` → 容器 → `.rvdlm`，语义相等
        // （张量数值 + 元数据键值对）。
        //
        // 为什么比**语义**而不是字节：`.rvdlm` 的元数据段由
        // ModelExporter::save 按 unordered_map 迭代顺序写出（见
        // RVDLModelFormat.hpp），而导入侧拿到的元数据来自容器的有序段 ——
        // 键值集合相同，写出顺序不保证相同。字节相等是更强的断言，
        // 它成立与否取决于 unordered_map 的迭代顺序，那不是本文件的职责；
        // 语义相等才是 round-trip 的真实要求。
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
                // 元数据契约 = **不丢失的单调超集**（见 gguf_meta_to_rvdlm 说明）：
                // 原键值全部完好；gguf 路径唯一可能多出的是 architecture
                // （导出侧为兼容 llama.cpp 补的默认值，反向映射无从区分）。
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

        // 7b：类型**自动**推导 —— 容器记录 F64 时不得按 f32 导入
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

        // 7c：手动指定 T 与容器 dtype 不一致 → 报错（不是猜）
        check(!import_safetensors_to_rvdlm<double>(st, g_out_dir + "/mismatch.rvdlm", "weight", &err),
              "容器是 F32 却按 double 导入 → 拒绝");
        check(err.find("类型不一致") != std::string::npos,
              "拒绝信息说明类型不一致（" + err + "）");

        // 7d：张量名不匹配 → 报错并列出可用名（多张量容器不逐个盲猜）
        check(!import_safetensors_to_rvdlm_auto(st, g_out_dir + "/noname.rvdlm", "nope", &err),
              "不存在的张量名 → 拒绝");
        check(err.find("可用") != std::string::npos, "拒绝信息列出可用张量名（" + err + "）");

        // 7e：多张量容器 —— 缺省取第一个，指名取指定那个
        {
            TensorData a; a.name = "alpha"; a.dtype = DType::F32; a.shape = {2};
            a.bytes.resize(8);
            float va[2] = {1.f, 2.f};
            std::memcpy(a.bytes.data(), va, 8);
            TensorData b = a; b.name = "beta"; b.shape = {3}; b.bytes.resize(12);
            float vb[3] = {7.f, 8.f, 9.f};
            std::memcpy(b.bytes.data(), vb, 12);
            const std::string multi = g_out_dir + "/multi.safetensors";
            // save_safetensors 只写单张量 —— 多张量文件按规范手工拼一个
            // （这也顺便证明 load 侧真的能解多张量头）。
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

        // 7f：u8/bool → GGUF 走不通（ggml 表里没有该类型）——
        // 反向同理：导出侧就失败，不存在可导入的 gguf；此处确认
        // GGUF 读到量化/未知类型时拒绝（不假装支持）。
        {
            std::vector<TensorData> ts;
            Metadata m;
            // 手写一个 gtype = 1（Q4_0 附近）的 gguf 张量信息段
            std::vector<u8> f;
            auto u32p = [&](u32 v) { for (int i = 0; i < 4; ++i) f.push_back((u8)((v >> (8 * i)) & 0xFF)); };
            auto u64p = [&](u64 v) { for (int i = 0; i < 8; ++i) f.push_back((u8)((v >> (8 * i)) & 0xFF)); };
            auto strp = [&](const std::string &s) { u64p(s.size()); f.insert(f.end(), s.begin(), s.end()); };
            f.insert(f.end(), {'G', 'G', 'U', 'F'});
            u32p(3);
            u64p(1);   // 1 个张量
            u64p(1);   // 1 个 KV
            strp("general.alignment");
            u32p(4);   // UINT32
            u32p(32);
            strp("w");
            u32p(1);
            u64p(2);
            u32p(1);   // gtype = 1（非四种受支持类型）
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