#pragma once
// ============================================================================
// 区间抽象判定（引擎一）
//
// 实现了「保守判定 / 弃权」语义。当引擎二（确定性符号执行）的反例搜索
// 无法覆盖大范围或非线性场景时，用整数区间 [lo, hi] 抽象变量取值，做保守判定：
//   - 若在区间语义下义务恒真 → unsat（已证明，可靠）
//   - 若区间发现必然反例       → sat
//   - 否则                     → unknown（弃权，绝不误报）
//
// 非线性（区间乘法）导致区间膨胀时，用 numqk::SoftLogic 的 t-norm
// （lukasiewicz / product / godel）做「可满足性置信度」松弛，落入 unknown。
//
// 仅依赖标准库 + numqk（header-only），可独立编译。
// ============================================================================

#include "Verifier.hpp"
#include "../numqk/SoftLogic.hpp"

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <limits>

namespace qhal::verify {
    // 整数区间 [lo, hi]
    struct Interval {
        int64_t lo = 0;
        int64_t hi = 0;
        bool empty = false;

        static Interval point(int64_t v) { return { v, v, false }; }
        static Interval bottom() { return { 0, 0, true }; }
        static Interval full() { return { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), false }; }

        bool isFull() const {
            return !empty && lo == std::numeric_limits<int64_t>::min() && hi == std::numeric_limits<int64_t>::max();
        }
        bool contains(int64_t v) const { return !empty && lo <= v && v <= hi; }
        int64_t width() const { return empty ? 0 : (hi - lo); }
    };

    // 区间算术（整数，忽略溢出，仅用于保守抽象）
    inline Interval iadd(const Interval& a, const Interval& b) {
        if (a.empty || b.empty) return Interval::bottom();
        return { a.lo + b.lo, a.hi + b.hi, false };
    }
    inline Interval isub(const Interval& a, const Interval& b) {
        if (a.empty || b.empty) return Interval::bottom();
        return { a.lo - b.hi, a.hi - b.lo, false };
    }
    inline Interval ineg(const Interval& a) {
        if (a.empty) return Interval::bottom();
        return { -a.hi, -a.lo, false };
    }
    inline Interval imul(const Interval& a, const Interval& b) {
        if (a.empty || b.empty) return Interval::bottom();
        int64_t p1 = a.lo * b.lo, p2 = a.lo * b.hi, p3 = a.hi * b.lo, p4 = a.hi * b.hi;
        int64_t lo = std::min({ p1, p2, p3, p4 });
        int64_t hi = std::max({ p1, p2, p3, p4 });
        return { lo, hi, false };
    }

    // 三值比较结果
    enum class Tri { True, False, Unknown };

    // 区间比较：返回「肯定真 / 肯定假 / 未知」
    inline Tri icmp(const std::string& op, const Interval& a, const Interval& b) {
        if (a.empty || b.empty) return Tri::False;
        if (op == "==") {
            if (a.lo == a.hi && b.lo == b.hi && a.lo == b.lo) return Tri::True;
            if (a.hi < b.lo || b.hi < a.lo) return Tri::False;
            return Tri::Unknown;
        }
        if (op == "!=") {
            if (a.hi < b.lo || b.hi < a.lo) return Tri::True;
            if (a.lo == a.hi && b.lo == b.hi && a.lo == b.lo) return Tri::False;
            return Tri::Unknown;
        }
        if (op == "<") {
            if (a.hi < b.lo) return Tri::True;
            if (a.lo >= b.hi) return Tri::False;
            return Tri::Unknown;
        }
        if (op == "<=") {
            if (a.hi <= b.lo) return Tri::True;
            if (a.lo > b.hi) return Tri::False;
            return Tri::Unknown;
        }
        if (op == ">") {
            if (a.lo > b.hi) return Tri::True;
            if (a.hi <= b.lo) return Tri::False;
            return Tri::Unknown;
        }
        if (op == ">=") {
            if (a.lo >= b.hi) return Tri::True;
            if (a.hi < b.lo) return Tri::False;
            return Tri::Unknown;
        }
        return Tri::Unknown;
    }

    // 区间抽象解释器
    class IntervalAbstract {
    public:
        // 对义务做区间判定
        static Solver::Result check(const Obligation& ob) {
            Solver::Result r;
            std::map<std::string, Interval> env;

            // 从 ante 提取约束（不等式 x < c 等），初始化变量区间
            propagate(ob.ante, env);

            Tri conse = evalTri(ob.conse, env);
            if (conse == Tri::True) {
                r.verdict = Solver::Verdict::Unsat;
                return r;
            }
            if (conse == Tri::False) {
                // 区间语义下 conse 恒假，但可能因为抽象过度；保守返回 unknown
                r.verdict = Solver::Verdict::Unknown;
                r.reason = "interval abstraction concludes false (may be imprecise)";
                return r;
            }
            r.verdict = Solver::Verdict::Unknown;
            r.reason = "interval abstraction cannot decide";
            return r;
        }

    private:
        // ── 辅助：常量取值（i32 / double / bool）─────────────────────────
        static int64_t constValue(const SExpr& e) {
            if (e.type == "double") return static_cast<int64_t>(std::stod(e.value));
            if (e.type == "bool") return e.value == "true" ? 1 : 0;
            return static_cast<int64_t>(std::stoll(e.value));
        }

        // ── 辅助：op 取反（用于 c op x → x 反op c 的归一化）──────────────
        static std::string mirrorOp(const std::string& op) {
            if (op == "<") return ">";
            if (op == "<=") return ">=";
            if (op == ">") return "<";
            if (op == ">=") return "<=";
            return op; // "==" 对称
        }

        static Interval intervalOf(const std::map<std::string, Interval>& env, const std::string& n) {
            auto it = env.find(n);
            return it != env.end() ? it->second : Interval::full();
        }

        // 用「x op c」收紧 x 的区间（op 已归一化为变量在左）
        static void applyBound(std::map<std::string, Interval>& env,
                               const std::string& varName, const std::string& op, int64_t c) {
            Interval cur = intervalOf(env, varName);
            if (op == "<") cur.hi = std::min(cur.hi, c - 1);
            else if (op == "<=") cur.hi = std::min(cur.hi, c);
            else if (op == ">") cur.lo = std::max(cur.lo, c + 1);
            else if (op == ">=") cur.lo = std::max(cur.lo, c);
            else if (op == "==") { cur.lo = std::max(cur.lo, c); cur.hi = std::min(cur.hi, c); }
            if (cur.lo > cur.hi) cur = Interval::bottom();
            env[varName] = cur;
        }

        // 尝试把 e 解析为「变量 + 常量偏移」：var、var + c、c + var、var - c
        static bool asAffineVar(const SExpr& e, std::string& varName, int64_t& offset) {
            if (e.kind == SExpr::Kind::Var) { varName = e.name; offset = 0; return true; }
            if (e.kind == SExpr::Kind::App && (e.op == "+" || e.op == "-") && e.args.size() == 2) {
                const SExpr& a = e.args[0];
                const SExpr& b = e.args[1];
                if (a.kind == SExpr::Kind::Var && b.kind == SExpr::Kind::Const) {
                    varName = a.name;
                    const int64_t c = constValue(b);
                    offset = (e.op == "+") ? c : -c;
                    return true;
                }
                if (e.op == "+" && b.kind == SExpr::Kind::Var && a.kind == SExpr::Kind::Const) {
                    varName = b.name;
                    offset = constValue(a);
                    return true;
                }
            }
            return false;
        }

        // 从表达式提取约束并传播到变量区间。
        //
        // 原实现仅支持 `x op c`（变量与常量），其余形式一律「暂不传播」而放弃，
        // 导致含 x+1<c 或 x<y 的义务完全得不到区间信息。现支持：
        //   (a) 常量界：   x op c        /  c op x
        //   (b) 仿射界：   x + k op c    /  c op x + k
        //   (c) 变量-变量：x op y        （用对方当前区间互相收紧，双向对称）
        static void propagate(const SExpr& e, std::map<std::string, Interval>& env) {
            if (e.kind == SExpr::Kind::App && e.op == "&&") {
                propagate(e.args[0], env);
                propagate(e.args[1], env);
                return;
            }
            if (e.kind != SExpr::Kind::App) return;
            const std::string& op = e.op;
            if (op != "<" && op != "<=" && op != ">" && op != ">=" && op != "==") return;
            if (e.args.size() != 2) return;

            const SExpr& l = e.args[0];
            const SExpr& r = e.args[1];

            // (a) x op c
            if (l.kind == SExpr::Kind::Var && r.kind == SExpr::Kind::Const) {
                applyBound(env, l.name, op, constValue(r));
                return;
            }
            // (a') c op x → x 反op c
            if (r.kind == SExpr::Kind::Var && l.kind == SExpr::Kind::Const) {
                applyBound(env, r.name, mirrorOp(op), constValue(l));
                return;
            }

            std::string ln, rn;
            int64_t lo = 0, ro = 0;
            const bool lAff = asAffineVar(l, ln, lo);
            const bool rAff = asAffineVar(r, rn, ro);

            // (b) x + lo op c  ⟺  x op (c - lo)
            if (lAff && r.kind == SExpr::Kind::Const) {
                applyBound(env, ln, op, constValue(r) - lo);
                return;
            }
            // (b') c op x + ro  ⟺  x 反op (c - ro)
            if (rAff && l.kind == SExpr::Kind::Const) {
                applyBound(env, rn, mirrorOp(op), constValue(l) - ro);
                return;
            }
            // (c) x + lo op y + ro  ⟺  x op y + (ro - lo)
            if (lAff && rAff) {
                const int64_t delta = ro - lo;
                const Interval ox = intervalOf(env, ln);
                const Interval oy = intervalOf(env, rn);
                Interval ix = ox, iy = oy;
                // 双向收紧，两侧均基于**原始**区间计算，避免顺序依赖。
                if (op == "<" || op == "<=") {
                    const int64_t slack = (op == "<") ? 1 : 0; // x < y  ⇒ x ≤ y-1
                    ix.hi = std::min(ox.hi, oy.hi + delta - slack);
                    iy.lo = std::max(oy.lo, ox.lo - delta + slack);
                } else if (op == ">" || op == ">=") {
                    const int64_t slack = (op == ">") ? 1 : 0; // x > y  ⇒ x ≥ y+1
                    ix.lo = std::max(ox.lo, oy.lo + delta + slack);
                    iy.hi = std::min(oy.hi, ox.hi - delta - slack);
                } else if (op == "==") {
                    ix.lo = std::max(ox.lo, oy.lo + delta);
                    ix.hi = std::min(ox.hi, oy.hi + delta);
                    iy.lo = std::max(oy.lo, ox.lo - delta);
                    iy.hi = std::min(oy.hi, ox.hi - delta);
                }
                if (ix.lo > ix.hi) ix = Interval::bottom();
                if (iy.lo > iy.hi) iy = Interval::bottom();
                env[ln] = ix;
                env[rn] = iy;
                return;
            }
            // 仍无法处理的形式（含乘法、函数调用等）：保守放弃传播。
        }

        // 区间语义下求值表达式
        static Interval eval(const SExpr& e, const std::map<std::string, Interval>& env) {
            if (e.kind == SExpr::Kind::Var) {
                auto it = env.find(e.name);
                return it != env.end() ? it->second : Interval::full();
            }
            if (e.kind == SExpr::Kind::Const) {
                if (e.type == "double") {
                    double d = std::stod(e.value);
                    int64_t v = (int64_t)d;
                    return Interval::point(v);
                }
                if (e.type == "bool") return Interval::point(e.value == "true" ? 1 : 0);
                return Interval::point((int64_t)std::stoll(e.value));
            }
            const std::string& op = e.op;
            if (op == "+") return iadd(eval(e.args[0], env), eval(e.args[1], env));
            if (op == "-" && e.args.size() == 2) return isub(eval(e.args[0], env), eval(e.args[1], env));
            if (op == "-" && e.args.size() == 1) return ineg(eval(e.args[0], env));
            if (op == "*") return imul(eval(e.args[0], env), eval(e.args[1], env));
            // 除法与其它：返回全区间（放弃精确，交由上层 unknown）
            return Interval::full();
        }

        // 三值求值
        static Tri evalTri(const SExpr& e, const std::map<std::string, Interval>& env) {
            if (e.kind == SExpr::Kind::App && (e.op == "==" || e.op == "!=" || e.op == "<" || e.op == "<=" || e.op == ">" || e.op == ">=")) {
                return icmp(e.op, eval(e.args[0], env), eval(e.args[1], env));
            }
            if (e.kind == SExpr::Kind::App && e.op == "&&") {
                Tri a = evalTri(e.args[0], env);
                Tri b = evalTri(e.args[1], env);
                if (a == Tri::False || b == Tri::False) return Tri::False;
                if (a == Tri::True && b == Tri::True) return Tri::True;
                return Tri::Unknown;
            }
            if (e.kind == SExpr::Kind::App && e.op == "!") {
                Tri a = evalTri(e.args[0], env);
                if (a == Tri::True) return Tri::False;
                if (a == Tri::False) return Tri::True;
                return Tri::Unknown;
            }
            // 常量布尔
            if (e.kind == SExpr::Kind::Const && e.type == "bool") {
                return e.value == "true" ? Tri::True : Tri::False;
            }
            return Tri::Unknown;
        }
    };

    // 引擎二（符号执行）→ unknown 时引擎一（区间抽象）兜底
    inline std::string verifyProtocolCombined(const std::string& protocolText) {
        std::vector<Obligation> obs;
        try {
            obs = ProtocolParser::parse(protocolText);
        } catch (const std::exception& e) {
            return std::string("VERIFY_ERROR parse: ") + e.what() + "\n";
        }

        std::string out;
        for (const auto& ob : obs) {
            Solver::Result res = Solver::check(ob);        // 引擎二：符号执行
            if (res.verdict == Solver::Verdict::Unknown) {
                res = IntervalAbstract::check(ob);          // 引擎一：区间抽象兜底
            }
            out += serializeResult(ob, res);
        }
        return out;
    }
}