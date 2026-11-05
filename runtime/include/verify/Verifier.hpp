#pragma once
// ============================================================================
// rvdl 静态证明判定器（确定性符号执行）
//
// 输入：VCGen（server/src/vcgen.ts）生成的行协议（S-表达式义务列表）。
// 输出：三值判定结果（sat / unsat / unknown），其中 sat 携带反例（见证输入）。
//
// 设计原则：
//   - 「弃权是特性」：无法判定时返回 unknown，绝不误报。
//   - 「驳斥需正面证明」：返回 sat 时附带具体反例（见证输入）。
//
// 仅依赖 C++ 标准库，不依赖 LLVM / Kokkos，可独立编译与单测。
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <set>

namespace qhal::verify {
    // ---------------------------------------------------------------------------
    // 值：整数 / 浮点 / 布尔 的统一表示
    // ---------------------------------------------------------------------------
    struct Value {
        enum class T { Int, Double, Bool } type = T::Int;
        int64_t i = 0;
        double d = 0.0;
        bool b = false;

        static Value makeInt(int64_t v) { Value x; x.type = T::Int; x.i = v; return x; }
        static Value makeDouble(double v) { Value x; x.type = T::Double; x.d = v; return x; }
        static Value makeBool(bool v) { Value x; x.type = T::Bool; x.b = v; return x; }
    };

    // ---------------------------------------------------------------------------
    // S-表达式
    // ---------------------------------------------------------------------------
    struct SExpr {
        enum class Kind { Var, Const, App } kind = Kind::Var;
        std::string name;    // Var: 变量名
        std::string type;    // Const: 类型（i32/double/bool/string/char）
        std::string value;   // Const: 字面值
        std::string op;      // App: 操作符
        std::vector<SExpr> args;
    };

    // ---------------------------------------------------------------------------
    // 义务：ANTE => CONSE
    // ---------------------------------------------------------------------------
    struct Obligation {
        std::string id;
        SExpr ante;
        SExpr conse;
    };

    // ---------------------------------------------------------------------------
    // 行协议解析器
    // ---------------------------------------------------------------------------
    class ProtocolParser {
    public:
        // 解析完整协议（OBLIGATION ... END_OBLIGATION 重复）
        static std::vector<Obligation> parse(const std::string& text) {
            std::vector<Obligation> out;
            std::istringstream ss(text);
            std::string line;
            Obligation cur;
            bool inOb = false;
            while (std::getline(ss, line)) {
                line = trim(line);
                if (line.empty()) continue;
                if (line.rfind("OBLIGATION ", 0) == 0) {
                    if (inOb) out.push_back(cur);
                    cur = Obligation{};
                    cur.id = line.substr(11);
                    inOb = true;
                } else if (line.rfind("ANTE ", 0) == 0) {
                    size_t p = 0;
                    cur.ante = parseSExpr(line.substr(5), p);
                } else if (line.rfind("CONSE ", 0) == 0) {
                    size_t p = 0;
                    cur.conse = parseSExpr(line.substr(6), p);
                } else if (line == "END_OBLIGATION") {
                    if (inOb) { out.push_back(cur); inOb = false; }
                }
            }
            if (inOb) out.push_back(cur);
            return out;
        }

    private:
        static std::string trim(const std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return "";
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        static void skipWs(const std::string& t, size_t& p) {
            while (p < t.size() && std::isspace((unsigned char)t[p])) p++;
        }

        static std::string readToken(const std::string& t, size_t& p) {
            skipWs(t, p);
            size_t s = p;
            while (p < t.size() && !std::isspace((unsigned char)t[p]) && t[p] != '(' && t[p] != ')') p++;
            return t.substr(s, p - s);
        }

        static SExpr parseSExpr(const std::string& t, size_t& p) {
            skipWs(t, p);
            if (p >= t.size() || t[p] != '(')
                throw std::runtime_error("Verifier: expected '(' in S-expression");
            p++; // '('
            std::string op = readToken(t, p);

            SExpr node;
            if (op == "var") {
                node.kind = SExpr::Kind::Var;
                node.name = readToken(t, p);
            } else if (op == "const") {
                node.kind = SExpr::Kind::Const;
                node.type = readToken(t, p);
                node.value = readToken(t, p);
            } else {
                node.kind = SExpr::Kind::App;
                node.op = op;
                while (true) {
                    skipWs(t, p);
                    if (p < t.size() && t[p] == ')') { p++; break; }
                    node.args.push_back(parseSExpr(t, p));
                }
                return node;
            }
            // 消费 ')'
            skipWs(t, p);
            if (p < t.size() && t[p] == ')') p++;
            return node;
        }
    };

    // ---------------------------------------------------------------------------
    // 符号执行求解器
    // ---------------------------------------------------------------------------
    class Solver {
    public:
        enum class Verdict { Sat, Unsat, Unknown };

        struct Result {
            Verdict verdict = Verdict::Unknown;
            std::map<std::string, Value> model;
            std::string reason;
        };

        // 对义务列表逐一判定
        static std::vector<std::pair<Obligation, Result>> checkAll(const std::vector<Obligation>& obs) {
            std::vector<std::pair<Obligation, Result>> out;
            for (const auto& ob : obs) out.push_back({ ob, check(ob) });
            return out;
        }

        // 验证 ANTE => CONSE
        static Result check(const Obligation& ob) {
            Result r;

            // 自反恒真快速路径（如 X == X、X <= X、true）
            if (isTriviallyTrue(ob.conse)) {
                r.verdict = Verdict::Unsat;
                return r;
            }

            // 收集自由变量
            std::set<std::string> vars;
            collectVars(ob.ante, vars);
            collectVars(ob.conse, vars);

            // 无自由变量：直接常量求值
            if (vars.empty()) {
                return checkGround(ob);
            }

            // CONSE 不含变量：可直接求值，避免无谓的反例搜索
            std::set<std::string> conseVars;
            collectVars(ob.conse, conseVars);
            if (conseVars.empty()) {
                try {
                    bool conseVal = toBool(eval(ob.conse, {}));
                    if (conseVal) {
                        // ante => true 恒真 → 已证明
                        r.verdict = Verdict::Unsat;
                        return r;
                    }
                    // conse 恒假：只需找 ante 可满足的反例
                    std::map<std::string, Value> model;
                    if (searchAntecedent(ob, vars, model)) {
                        r.verdict = Verdict::Sat;
                        r.model = model;
                        return r;
                    }
                    r.verdict = Verdict::Unknown;
                    r.reason = "cannot establish satisfiability of antecedent";
                    return r;
                } catch (...) {
                    // 求值失败则回退到通用反例搜索
                }
            }

            // 通用反例搜索：找使 ante 真且 conse 假的赋值
            std::map<std::string, Value> model;
            if (searchCounterexample(ob, vars, model)) {
                r.verdict = Verdict::Sat;
                r.model = model;
                return r;
            }

            // 无法找到反例，也无法证明恒真 → 弃权
            r.verdict = Verdict::Unknown;
            r.reason = "no counterexample found within search bounds (soundness not guaranteed)";
            return r;
        }

    private:
        // 无变量义务的精确判定
        static Result checkGround(const Obligation& ob) {
            Result r;
            std::map<std::string, Value> env;
            bool anteVal, conseVal;
            try {
                anteVal = toBool(eval(ob.ante, env));
                conseVal = toBool(eval(ob.conse, env));
            } catch (...) {
                r.verdict = Verdict::Unknown;
                r.reason = "ground evaluation failed";
                return r;
            }
            // ante => conse 恒真当且仅当 !ante || conse
            if (!anteVal || conseVal) {
                r.verdict = Verdict::Unsat; // 已证明
            } else {
                r.verdict = Verdict::Sat;   // ante 真但 conse 假 → 反例
            }
            return r;
        }

        // 前件可满足性搜索：找使 ante 为真的赋值（用于 conse 恒假时）
        static bool searchAntecedent(const Obligation& ob,
                                    const std::set<std::string>& vars,
                                    std::map<std::string, Value>& model) {
            const int MAX_VARS = 4;
            const int64_t LO = -8, HI = 8;
            if ((int)vars.size() > MAX_VARS) return false;
            std::vector<std::string> vlist(vars.begin(), vars.end());
            std::map<std::string, Value> env;
            return dfsAnte(ob, vlist, 0, env, model, LO, HI);
        }

        static bool dfsAnte(const Obligation& ob,
                            const std::vector<std::string>& vlist,
                            size_t idx,
                            std::map<std::string, Value>& env,
                            std::map<std::string, Value>& model,
                            int64_t lo, int64_t hi) {
            if (idx == vlist.size()) {
                try {
                    if (toBool(eval(ob.ante, env))) { model = env; return true; }
                } catch (...) { return false; }
                return false;
            }
            const std::string& name = vlist[idx];
            for (int64_t v = lo; v <= hi; ++v) {
                env[name] = Value::makeInt(v);
                if (dfsAnte(ob, vlist, idx + 1, env, model, lo, hi)) return true;
            }
            env.erase(name);
            return false;
        }

        // 反例搜索：枚举自由变量，找使 ante 真且 conse 假的赋值
        static bool searchCounterexample(const Obligation& ob,
                                        const std::set<std::string>& vars,
                                        std::map<std::string, Value>& model) {
            // 限制变量数量与枚举范围，避免组合爆炸
            const int MAX_VARS = 4;
            const int64_t LO = -8, HI = 8;
            if ((int)vars.size() > MAX_VARS) return false;

            std::vector<std::string> vlist(vars.begin(), vars.end());
            std::map<std::string, Value> env;
            return dfs(ob, vlist, 0, env, model, LO, HI);
        }

        static bool dfs(const Obligation& ob,
                        const std::vector<std::string>& vlist,
                        size_t idx,
                        std::map<std::string, Value>& env,
                        std::map<std::string, Value>& model,
                        int64_t lo, int64_t hi) {
            if (idx == vlist.size()) {
                try {
                    bool anteVal = toBool(eval(ob.ante, env));
                    if (!anteVal) return false;              // 前提不成立，跳过
                    bool conseVal = toBool(eval(ob.conse, env));
                    if (!conseVal) { model = env; return true; } // 反例！
                } catch (...) { return false; }
                return false;
            }
            const std::string& name = vlist[idx];
            for (int64_t v = lo; v <= hi; ++v) {
                env[name] = Value::makeInt(v);
                if (dfs(ob, vlist, idx + 1, env, model, lo, hi)) return true;
            }
            env.erase(name);
            return false;
        }

        // 收集变量
        static void collectVars(const SExpr& e, std::set<std::string>& vars) {
            if (e.kind == SExpr::Kind::Var) { vars.insert(e.name); return; }
            if (e.kind == SExpr::Kind::App) {
                for (const auto& a : e.args) collectVars(a, vars);
            }
        }

        // 结构相等性
        static bool sexprEqual(const SExpr& a, const SExpr& b) {
            if (a.kind != b.kind) return false;
            if (a.kind == SExpr::Kind::Var) return a.name == b.name;
            if (a.kind == SExpr::Kind::Const) return a.type == b.type && a.value == b.value;
            if (a.op != b.op || a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); ++i)
                if (!sexprEqual(a.args[i], b.args[i])) return false;
            return true;
        }

        // 自反恒真：true / X == X / X <= X / X >= X / 合取的递归
        static bool isTriviallyTrue(const SExpr& e) {
            if (e.kind == SExpr::Kind::Const && e.type == "bool" && e.value == "true") return true;
            if (e.kind == SExpr::Kind::App) {
                if ((e.op == "==" || e.op == "<=" || e.op == ">=") && e.args.size() == 2) {
                    if (sexprEqual(e.args[0], e.args[1])) return true;
                }
                if (e.op == "&&") {
                    return isTriviallyTrue(e.args[0]) && isTriviallyTrue(e.args[1]);
                }
            }
            return false;
        }

        // -----------------------------------------------------------------------
        // 求值
        // -----------------------------------------------------------------------
        static Value eval(const SExpr& e, const std::map<std::string, Value>& env) {
            if (e.kind == SExpr::Kind::Var) {
                auto it = env.find(e.name);
                if (it == env.end()) throw std::runtime_error("unbound var " + e.name);
                return it->second;
            }
            if (e.kind == SExpr::Kind::Const) {
                return parseConst(e);
            }
            // App
            const std::string& op = e.op;
            if (op == "!") {
                return Value::makeBool(!toBool(eval(e.args[0], env)));
            }
            if (op == "-" && e.args.size() == 1) {
                Value a = eval(e.args[0], env);
                if (a.type == Value::T::Double) return Value::makeDouble(-a.d);
                return Value::makeInt(-a.i);
            }
            if (op == "&&") {
                bool l = toBool(eval(e.args[0], env));
                if (!l) return Value::makeBool(false);
                return Value::makeBool(toBool(eval(e.args[1], env)));
            }
            if (op == "||") {
                bool l = toBool(eval(e.args[0], env));
                if (l) return Value::makeBool(true);
                return Value::makeBool(toBool(eval(e.args[1], env)));
            }

            Value l = eval(e.args[0], env);
            Value r = eval(e.args[1], env);

            if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
                bool res = compare(op, l, r);
                return Value::makeBool(res);
            }
            return arith(op, l, r);
        }

        static Value parseConst(const SExpr& e) {
            if (e.type == "double") return Value::makeDouble(std::stod(e.value));
            if (e.type == "bool") return Value::makeBool(e.value == "true");
            // i32 / i64 / i8 / i16 等一律视为整数
            return Value::makeInt((int64_t)std::stoll(e.value));
        }

        static bool toBool(const Value& v) {
            if (v.type == Value::T::Bool) return v.b;
            return v.i != 0;
        }

        static bool isNum(const Value& v) { return v.type == Value::T::Int || v.type == Value::T::Double; }

        static bool compare(const std::string& op, const Value& l, const Value& r) {
            double a = l.type == Value::T::Double ? l.d : (double)l.i;
            double b = r.type == Value::T::Double ? r.d : (double)r.i;
            if (op == "==") return a == b;
            if (op == "!=") return a != b;
            if (op == "<") return a < b;
            if (op == "<=") return a <= b;
            if (op == ">") return a > b;
            if (op == ">=") return a >= b;
            throw std::runtime_error("unknown cmp op " + op);
        }

        static Value arith(const std::string& op, const Value& l, const Value& r) {
            bool fp = (l.type == Value::T::Double || r.type == Value::T::Double);
            if (fp) {
                double a = l.type == Value::T::Double ? l.d : (double)l.i;
                double b = r.type == Value::T::Double ? r.d : (double)r.i;
                if (op == "+") return Value::makeDouble(a + b);
                if (op == "-") return Value::makeDouble(a - b);
                if (op == "*") return Value::makeDouble(a * b);
                if (op == "/") {
                    if (b == 0.0) throw std::runtime_error("division by zero");
                    return Value::makeDouble(a / b);
                }
                throw std::runtime_error("unknown arith op " + op);
            }
            int64_t a = l.i, b = r.i;
            if (op == "+") return Value::makeInt(a + b);
            if (op == "-") return Value::makeInt(a - b);
            if (op == "*") return Value::makeInt(a * b);
            if (op == "/") {
                if (b == 0) throw std::runtime_error("division by zero");
                // 截断除法（与 LLVM sdiv 一致）
                return Value::makeInt(a / b);
            }
            throw std::runtime_error("unknown arith op " + op);
        }
    };

    // ---------------------------------------------------------------------------
    // 高层入口：把协议文本判定为可读的结果文本
    // ---------------------------------------------------------------------------
    // 序列化单个义务的判定结果
    inline std::string serializeResult(const Obligation& ob, const Solver::Result& res) {
        std::string out = "OBLIGATION " + ob.id + "\n";
        switch (res.verdict) {
            case Solver::Verdict::Unsat:
                out += "VERDICT unsat\n";
                break;
            case Solver::Verdict::Sat:
                out += "VERDICT sat\n";
                for (const auto& [name, val] : res.model) {
                    out += "MODEL " + name + "=";
                    if (val.type == Value::T::Int) out += "i32:" + std::to_string(val.i);
                    else if (val.type == Value::T::Double) out += "double:" + std::to_string(val.d);
                    else out += "bool:" + std::string(val.b ? "true" : "false");
                    out += "\n";
                }
                break;
            default:
                out += "VERDICT unknown\n";
                out += "REASON " + res.reason + "\n";
                break;
        }
        out += "END_OBLIGATION\n";
        return out;
    }

    // 单引擎入口（符号执行）
    inline std::string verifyProtocol(const std::string& protocolText) {
        std::vector<Obligation> obs;
        try {
            obs = ProtocolParser::parse(protocolText);
        } catch (const std::exception& e) {
            return std::string("VERIFY_ERROR parse: ") + e.what() + "\n";
        }

        std::string out;
        for (const auto& ob : obs) {
            out += serializeResult(ob, Solver::check(ob));
        }
        return out;
    }
}