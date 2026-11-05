#pragma once
//
// rvdl_lang.hpp —— rvdl 语言构件定义与真实门序列生成
//
// 门序列生成：
//   • 解析 rvdl 源码中的量子门调用（h/x/rz/cnot/toffoli/swap/measure/alloc）
//   • Codegen<RealQuantumDevice> —— 生成真实设备的门调度（分配 → 门 → 测量）
//   • Codegen<QuantumVirtualMachine> —— 生成 QVM 的 C++ 调用序列
//
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <cctype>
#include <iostream>

namespace vedaros::rvdl
{

    // ─── rvdl 语言可定义的意识构件 ─────────────────────────────
    enum class MentalConstruct
    {
        Consciousness, // 意识
        Thought,       // 思想
        Decision,      // 决策
        Message,       // 消息
        Service,       // 服务
        Action         // 动作
    };

    // 编译目标
    enum class CodegenTarget
    {
        RealQuantumDevice,          // 真实量子设备 → 门序列
        QuantumVirtualMachine       // QVM → C++ 代码
    };

    // 由 rvdl 源定义的类型化构件
    struct QkDefinition
    {
        MentalConstruct kind = MentalConstruct::Message;
        std::string name;
        std::string rvdl_source;   // rvdl 语言片段
        std::string type_schema; // 类型模式（保证跨语言类型安全）
    };

    // ─── 单个门操作 ──────────────────────────────────────────
    struct GateOp
    {
        std::string name;            // h / x / rz / cnot / toffoli / swap / measure / alloc
        std::vector<int> qubits;     // 涉及的量子比特（控制/目标）
        double angle = 0.0;          // rz 的角度
    };

    // 按意识构件种类生成 C++ 声明/定义名
    static const char *cpp_construct(MentalConstruct c)
    {
        switch (c)
        {
        case MentalConstruct::Consciousness: return "Consciousness";
        case MentalConstruct::Thought:        return "Thought";
        case MentalConstruct::Decision:       return "Decision";
        case MentalConstruct::Message:        return "Message";
        case MentalConstruct::Service:        return "Service";
        case MentalConstruct::Action:         return "Action";
        }
        return "Unknown";
    }

    static const char *construct_name(MentalConstruct c)
    {
        switch (c)
        {
        case MentalConstruct::Consciousness: return "consciousness";
        case MentalConstruct::Thought:        return "thought";
        case MentalConstruct::Decision:       return "decision";
        case MentalConstruct::Message:        return "message";
        case MentalConstruct::Service:        return "service";
        case MentalConstruct::Action:         return "action";
        }
        return "unknown";
    }

    // ─── 门名判定 ────────────────────────────────────────────
    static bool is_gate_name(const std::string &name)
    {
        static const char *GATES[] = {"h", "x", "rz", "cnot", "toffoli", "swap", "measure", "alloc"};
        for (auto g : GATES)
            if (name == g) return true;
        return false;
    }

    // ─── rvdl 源码 → 门序列（轻量解析）─────────────────────────
    //
    // 识别形如 `h(q)`、`x(q)`、`rz(q, angle)`、`cnot(c, t)`、
    // `toffoli(c1, c2, t)`、`swap(a, b)`、`measure(q)`、`alloc(q)` 的门调用。
    // 参数为整数（量子比特索引）或浮点（角度）。
    static std::vector<GateOp> parse_gates(const std::string &src)
    {
        std::vector<GateOp> ops;
        size_t i = 0;
        const size_t n = src.size();
        while (i < n)
        {
            // 跳过空白与注释
            while (i < n && (std::isspace(static_cast<unsigned char>(src[i]))))
                ++i;
            if (i + 1 < n && src[i] == '/' && src[i + 1] == '/')
            {
                while (i < n && src[i] != '\n')
                    ++i;
                continue;
            }
            // 读标识符
            size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_'))
                ++i;
            if (i == start)
            {
                ++i;
                continue;
            }
            std::string name = src.substr(start, i - start);
            // 跳过空白，找 '('
            size_t j = i;
            while (j < n && std::isspace(static_cast<unsigned char>(src[j])))
                ++j;
            if (j >= n || src[j] != '(')
            {
                i = j;
                continue;
            }
            // 收集到匹配的 ')'
            size_t k = j + 1;
            std::string args;
            int depth = 1;
            while (k < n && depth > 0)
            {
                char ch = src[k];
                if (ch == '(') depth++;
                else if (ch == ')') depth--;
                if (depth > 0) args += ch;
                ++k;
            }
            if (!is_gate_name(name))
            {
                i = k;
                continue;
            }
            GateOp op;
            op.name = name;
            // 分割参数
            std::string cur;
            for (char ch : args)
            {
                if (ch == ',')
                {
                    if (!cur.empty())
                    {
                        if (name == "rz" && op.qubits.empty() == false && op.angle == 0.0 && cur.find('.') != std::string::npos)
                        {
                            op.angle = std::stod(cur);
                        }
                        else if (op.qubits.empty() && std::isdigit(static_cast<unsigned char>(cur[0])))
                        {
                            op.qubits.push_back(std::stoi(cur));
                        }
                        else if (!op.qubits.empty())
                        {
                            op.qubits.push_back(std::stoi(cur));
                        }
                        cur.clear();
                    }
                    continue;
                }
                if (!std::isspace(static_cast<unsigned char>(ch)))
                    cur += ch;
            }
            if (!cur.empty())
            {
                if (name == "rz" && op.qubits.size() >= 1)
                    op.angle = std::stod(cur);
                else
                    op.qubits.push_back(std::stoi(cur));
            }
            ops.push_back(op);
            i = k;
        }
        return ops;
    }

    // ─── 按构件种类的默认门序列（rvdl_source 为空时使用）──────
    static std::vector<GateOp> default_gates(MentalConstruct c)
    {
        std::vector<GateOp> ops;
        switch (c)
        {
        case MentalConstruct::Message:
            // 消息编码：制备叠加态 → 纠缠 → 测量
            ops.push_back({"h", {0}, 0.0});
            ops.push_back({"cnot", {0, 1}, 0.0});
            ops.push_back({"measure", {0}, 0.0});
            break;
        case MentalConstruct::Decision:
            // 决策：多比特纠缠 + 测量
            ops.push_back({"h", {0}, 0.0});
            ops.push_back({"cnot", {0, 1}, 0.0});
            ops.push_back({"cnot", {1, 2}, 0.0});
            ops.push_back({"measure", {2}, 0.0});
            break;
        case MentalConstruct::Action:
            // 动作：翻转 + 旋转
            ops.push_back({"x", {0}, 0.0});
            ops.push_back({"rz", {0}, 0.7853981634});
            ops.push_back({"measure", {0}, 0.0});
            break;
        default:
            ops.push_back({"h", {0}, 0.0});
            ops.push_back({"measure", {0}, 0.0});
            break;
        }
        return ops;
    }

    // ─── 编译时代码生成（按目标分派）─────────────────────────
    template <CodegenTarget Target>
    struct Codegen;

    // ─── QVM 目标：生成 C++ 调用序列 ──────────────────────────
    template <>
    struct Codegen<CodegenTarget::QuantumVirtualMachine>
    {
        static std::string generate(const QkDefinition &def)
        {
            std::vector<GateOp> gates = parse_gates(def.rvdl_source);
            if (gates.empty())
                gates = default_gates(def.kind);

            std::stringstream o;
            o << "// ── [vedaRos QVM] " << construct_name(def.kind) << " '" << def.name
              << "' (schema: " << def.type_schema << ") ──\n";
            o << "void " << def.name << "_" << cpp_construct(def.kind)
              << "(qhal::IQuantumBackend* backend) {\n";
            for (const auto &g : gates)
            {
                if (g.name == "alloc")
                    o << "    backend->allocate_qubits(" << (g.qubits.empty() ? 1 : g.qubits[0]) << ");\n";
                else if (g.name == "h")
                    o << "    backend->apply_h(" << g.qubits[0] << ");\n";
                else if (g.name == "x")
                    o << "    backend->apply_x(" << g.qubits[0] << ");\n";
                else if (g.name == "rz")
                    o << "    backend->apply_rz(" << g.qubits[0] << ", " << g.angle << ");\n";
                else if (g.name == "cnot")
                    o << "    backend->apply_cnot(" << g.qubits[0] << ", " << g.qubits[1] << ");\n";
                else if (g.name == "toffoli")
                    o << "    backend->apply_toffoli(" << g.qubits[0] << ", " << g.qubits[1] << ", " << g.qubits[2] << ");\n";
                else if (g.name == "swap")
                    o << "    backend->apply_swap(" << g.qubits[0] << ", " << g.qubits[1] << ");\n";
                else if (g.name == "measure")
                    o << "    backend->measure(" << g.qubits[0] << ");\n";
            }
            o << "}\n";
            return o.str();
        }
    };

    // ─── 真实量子设备目标：生成门调度 ────────────────────────
    template <>
    struct Codegen<CodegenTarget::RealQuantumDevice>
    {
        static std::string generate(const QkDefinition &def)
        {
            std::vector<GateOp> gates = parse_gates(def.rvdl_source);
            if (gates.empty())
                gates = default_gates(def.kind);

            std::stringstream o;
            o << "// ── [vedaRos Real-Device] " << construct_name(def.kind) << " '" << def.name
              << "' (schema: " << def.type_schema << ") ──\n";
            o << "gate_schedule " << def.name << " = {\n";
            int t = 0;
            for (const auto &g : gates)
            {
                o << "  t" << t++ << ": " << g.name;
                for (size_t k = 0; k < g.qubits.size(); ++k)
                {
                    o << (k == 0 ? "(" : ", ") << "q" << g.qubits[k];
                }
                if (g.name == "rz")
                    o << ", θ=" << g.angle;
                o << (g.qubits.empty() ? "" : ")") << ";\n";
            }
            o << "  measure: " << (gates.empty() ? "q0" : "q" + std::to_string(gates.back().qubits.empty() ? 0 : gates.back().qubits[0]))
              << ";\n";
            o << "};\n";
            return o.str();
        }
    };

    // ─── 类型安全注册表（跨语言、跨节点）─────────────────────
    class QkRegistry
    {
    private:
        mutable std::mutex mtx;
        std::unordered_map<std::string, QkDefinition> defs;

    public:
        void define(const QkDefinition &def)
        {
            std::lock_guard<std::mutex> lock(mtx);
            defs[def.name] = def;
            std::cout << "[vedaRos.rvdl] Defined " << construct_name(def.kind)
                      << " '" << def.name << "' (schema: " << def.type_schema << ").\n";
        }

        template <CodegenTarget Target>
        std::string codegen(const std::string &name) const
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = defs.find(name);
            if (it == defs.end())
                return "// [vedaRos.rvdl] definition '" + name + "' not found\n";
            return Codegen<Target>::generate(it->second);
        }

        std::vector<GateOp> gates(const std::string &name) const
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = defs.find(name);
            if (it == defs.end())
                return {};
            auto ops = parse_gates(it->second.rvdl_source);
            if (ops.empty())
                ops = default_gates(it->second.kind);
            return ops;
        }

        size_t size() const
        {
            std::lock_guard<std::mutex> lock(mtx);
            return defs.size();
        }
    };

} // namespace vedaros::rvdl
