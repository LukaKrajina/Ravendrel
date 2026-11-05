#pragma once
// ============================================================================
// SMT-LIB 接口预留（引擎三）
//
// 当自研判定器（引擎一 符号执行 + 引擎二 区间抽象）返回 unknown 时，可把验证
// 条件转成 SMT-LIB2 文本，交给外部成熟求解器（Z3 / cvc5）判定。保持与
// 引擎一和二 一致的 SatResult 三值语义，实现「无缝切换成熟求解器」的退路。
//
// 本模块把验证条件转成 (assert (and P (not Q))) + (check-sat)：
//   - unsat  → 已证明（P => Q 恒真）
//   - sat    → 反例（P 真且 Q 假）
//
// 仅依赖标准库 + Verifier.hpp，可独立编译。
// ============================================================================

#include "Verifier.hpp"

#include <string>
#include <vector>
#include <set>

namespace qhal::verify {
    class SmtLibEmitter {
    public:
        // 单义务 → SMT-LIB 脚本片段（declare-const + assert + check-sat）
        static std::string obligationToSmt(const Obligation& ob) {
            std::set<std::string> vars;
            collectVars(ob.ante, vars);
            collectVars(ob.conse, vars);

            std::string out = "; obligation " + ob.id + "\n";
            for (const auto& v : vars) {
                out += "(declare-const " + v + " Int)\n";
            }
            out += "(assert (and " + sexprToSmt(ob.ante) + " (not " + sexprToSmt(ob.conse) + ")))\n";
            out += "(check-sat)\n";
            return out;
        }

        // 多义务 → 完整 SMT-LIB 脚本
        static std::string obligationsToSmt(const std::vector<Obligation>& obs) {
            std::string out = "(set-logic ALL)\n\n";
            for (const auto& ob : obs) {
                out += obligationToSmt(ob);
                out += "\n";
            }
            return out;
        }

        // S-表达式 → SMT-LIB 表达式
        static std::string sexprToSmt(const SExpr& e) {
            if (e.kind == SExpr::Kind::Var) return e.name;
            if (e.kind == SExpr::Kind::Const) {
                return e.value; // true/false 或数字字面量
            }
            const std::string& op = e.op;

            // 一元
            if (op == "!") return "(not " + sexprToSmt(e.args[0]) + ")";
            if (op == "-" && e.args.size() == 1) return "(- " + sexprToSmt(e.args[0]) + ")";

            // 二元：运算符映射
            std::string smtOp;
            if (op == "==") smtOp = "=";
            else if (op == "!=") return "(not (= " + sexprToSmt(e.args[0]) + " " + sexprToSmt(e.args[1]) + "))";
            else if (op == "&&") smtOp = "and";
            else if (op == "||") smtOp = "or";
            else if (op == "+") smtOp = "+";
            else if (op == "-") smtOp = "-";
            else if (op == "*") smtOp = "*";
            else if (op == "/") smtOp = "div"; // 整数除法（截断）
            else if (op == "<") smtOp = "<";
            else if (op == "<=") smtOp = "<=";
            else if (op == ">") smtOp = ">";
            else if (op == ">=") smtOp = ">=";
            else smtOp = op;

            std::string out = "(" + smtOp;
            for (const auto& a : e.args) out += " " + sexprToSmt(a);
            out += ")";
            return out;
        }

    private:
        static void collectVars(const SExpr& e, std::set<std::string>& vars) {
            if (e.kind == SExpr::Kind::Var) { vars.insert(e.name); return; }
            if (e.kind == SExpr::Kind::App) {
                for (const auto& a : e.args) collectVars(a, vars);
            }
        }
    };
}