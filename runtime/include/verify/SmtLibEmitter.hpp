#pragma once

#include "Verifier.hpp"

#include <string>
#include <vector>
#include <set>

namespace qhal::verify {
    class SmtLibEmitter {
    public:
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

        static std::string obligationsToSmt(const std::vector<Obligation>& obs) {
            std::string out = "(set-logic ALL)\n\n";
            for (const auto& ob : obs) {
                out += obligationToSmt(ob);
                out += "\n";
            }
            return out;
        }

        static std::string sexprToSmt(const SExpr& e) {
            if (e.kind == SExpr::Kind::Var) return e.name;
            if (e.kind == SExpr::Kind::Const) {
                return e.value;
            }
            const std::string& op = e.op;

            if (op == "!") return "(not " + sexprToSmt(e.args[0]) + ")";
            if (op == "-" && e.args.size() == 1) return "(- " + sexprToSmt(e.args[0]) + ")";

            std::string smtOp;
            if (op == "==") smtOp = "=";
            else if (op == "!=") return "(not (= " + sexprToSmt(e.args[0]) + " " + sexprToSmt(e.args[1]) + "))";
            else if (op == "&&") smtOp = "and";
            else if (op == "||") smtOp = "or";
            else if (op == "+") smtOp = "+";
            else if (op == "-") smtOp = "-";
            else if (op == "*") smtOp = "*";
            else if (op == "/") smtOp = "div";
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