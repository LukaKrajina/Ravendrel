// ============================================================================
// qhal::json —— 轻量 JSON 解析 / 序列化（纯标准库实现，无 LLVM / JIT 依赖）
//
// 从 MMI.hpp 提取，独立成头文件，供 MirModuleBuilder / MMI 等复用，
// 避免了 MirModuleBuilder 因 MMI.hpp 的 JIT（vendor）依赖而无法独立编译。
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <map>
#include <utility>

namespace qhal
{
    namespace json
    {
        struct Value;
        using Array = std::vector<Value>;
        using Object = std::map<std::string, Value>;
        struct Value
        {
            enum Kind
            {
                Null,
                Bool,
                Number,
                Str,
                Arr,
                Obj
            } kind = Null;

            bool b = false;
            double num = 0;
            int64_t inum = 0;
            bool is_int = false;
            std::string str;
            Array arr;
            Object obj;
            Value() : kind(Null) {}
            Value(bool v) : kind(Bool), b(v) {}
            Value(int v) : kind(Number), num((double)v), inum((int64_t)v), is_int(true) {}
            Value(long long v) : kind(Number), num((double)v), inum((int64_t)v), is_int(true) {}
            Value(unsigned long long v) : kind(Number), num((double)v), inum((int64_t)v), is_int(true) {}
            Value(double v) : kind(Number), num(v) {}
            Value(float v) : kind(Number), num((double)v) {}
            Value(const char *s) : kind(Str), str(s ? s : "") {}
            Value(const std::string &s) : kind(Str), str(s) {}
            Value(Array a) : kind(Arr), arr(std::move(a)) {}
            Value(Object o) : kind(Obj), obj(std::move(o)) {}
            bool is_null() const { return kind == Null; }
            bool is_bool() const { return kind == Bool; }
            bool is_number() const { return kind == Number; }
            bool is_string() const { return kind == Str; }
            bool is_array() const { return kind == Arr; }
            bool is_object() const { return kind == Obj; }
            bool bool_value() const { return b; }
            double number() const { return is_int ? (double)inum : num; }
            int int_value() const { return (int)(is_int ? inum : (int64_t)num); }
            int64_t int64_value() const { return is_int ? inum : (int64_t)num; }
            const std::string &string() const { return str; }
            const Array &array() const { return arr; }
            Array &array() { return arr; }
            const Object &object() const { return obj; }
            Object &object() { return obj; }
            bool has(const std::string &key) const { return obj.count(key) != 0; }
            const Value &at(const std::string &key) const
            {
                auto it = obj.find(key);
                if (it == obj.end())
                    throw std::runtime_error("JSON: missing key '" + key + "'");
                return it->second;
            }
            Value &at(const std::string &key)
            {
                auto it = obj.find(key);
                if (it == obj.end())
                    throw std::runtime_error("JSON: missing key '" + key + "'");
                return it->second;
            }
            const Value &operator[](size_t i) const { return arr[i]; }
            Value &operator[](size_t i) { return arr[i]; }
        };

        static int hex_val(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        static void append_utf8(std::string &out, uint32_t cp)
        {
            if (cp <= 0x7F)
            {
                out += (char)cp;
            }
            else if (cp <= 0x7FF)
            {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else if (cp <= 0xFFFF)
            {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else
            {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }

        inline std::string escape_string(const std::string &s)
        {
            static const char hex_digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(s.size() + 2);
            out += '"';
            for (unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[7] = {'\\', 'u', '0', '0', '0', '0', '\0'};
                        buf[4] = hex_digits[(c >> 4) & 0xF];
                        buf[5] = hex_digits[c & 0xF];
                        out += buf;
                    }
                    else
                        out += (char)c;
                }
            }
            out += '"';
            return out;
        }

        inline std::string format_number(const Value &v)
        {
            std::ostringstream os;
            if (v.is_int)
            {
                os << v.inum;
            }
            else
            {
                os.precision(17);
                os << v.num;
            }
            return os.str();
        }

        inline void serialize_impl(const Value &v, std::string &out, int indent, int level)
        {
            switch (v.kind)
            {
            case Value::Null:
                out += "null";
                return;
            case Value::Bool:
                out += (v.b ? "true" : "false");
                return;
            case Value::Number:
                out += format_number(v);
                return;
            case Value::Str:
                out += escape_string(v.str);
                return;
            case Value::Arr:
            {
                out += '[';
                if (!v.arr.empty())
                {
                    for (size_t i = 0; i < v.arr.size(); ++i)
                    {
                        if (i > 0) out += ',';
                        if (indent >= 0)
                        {
                            out += '\n';
                            out.append((size_t)(indent * (level + 1)), ' ');
                        }
                        serialize_impl(v.arr[i], out, indent, level + 1);
                    }
                    if (indent >= 0)
                    {
                        out += '\n';
                        out.append((size_t)(indent * level), ' ');
                    }
                }
                out += ']';
                return;
            }
            case Value::Obj:
            {
                out += '{';
                if (!v.obj.empty())
                {
                    size_t i = 0;
                    for (const auto &kv : v.obj)
                    {
                        if (i > 0) out += ',';
                        if (indent >= 0)
                        {
                            out += '\n';
                            out.append((size_t)(indent * (level + 1)), ' ');
                        }
                        out += escape_string(kv.first);
                        out += (indent >= 0 ? ": " : ":");
                        serialize_impl(kv.second, out, indent, level + 1);
                        ++i;
                    }
                    if (indent >= 0)
                    {
                        out += '\n';
                        out.append((size_t)(indent * level), ' ');
                    }
                }
                out += '}';
                return;
            }
            }
        }

        inline std::string serialize(const Value &v, int indent = -1)
        {
            std::string out;
            serialize_impl(v, out, indent, 0);
            return out;
        }

        inline std::string stringify(const Value &v, int indent = -1) { return serialize(v, indent); }

        struct Parser
        {
            const std::string &s;
            size_t p = 0;

            Parser(const std::string &src) : s(src) {}

            void skip_ws()
            {
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
                    ++p;
            }
            char peek()
            {
                skip_ws();
                return p < s.size() ? s[p] : '\0';
            }
            char get()
            {
                skip_ws();
                return p < s.size() ? s[p++] : '\0';
            }

            Value parse_value()
            {
                Value v;
                char c = peek();
                if (c == '{')
                    v = parse_object();
                else if (c == '[')
                    v = parse_array();
                else if (c == '"')
                {
                    v.kind = Value::Str;
                    v.str = parse_string();
                }
                else if (c == 't')
                {
                    expect("true");
                    v.kind = Value::Bool;
                    v.b = true;
                }
                else if (c == 'f')
                {
                    expect("false");
                    v.kind = Value::Bool;
                    v.b = false;
                }
                else if (c == 'n')
                {
                    expect("null");
                    v.kind = Value::Null;
                }
                else
                {
                    v.kind = Value::Number;
                    v.num = parse_number(v.is_int, v.inum);
                }
                return v;
            }

            Value parse_object()
            {
                Value v;
                v.kind = Value::Obj;
                get(); // '{'
                if (peek() == '}')
                {
                    get();
                    return v;
                }
                while (true)
                {
                    std::string key = parse_string();
                    if (get() != ':')
                        throw std::runtime_error("JSON: expected ':'");
                    v.obj[key] = parse_value();
                    char c = get();
                    if (c == '}')
                        break;
                    if (c != ',')
                        throw std::runtime_error("JSON: expected ',' or '}'");
                }
                return v;
            }

            Value parse_array()
            {
                Value v;
                v.kind = Value::Arr;
                get(); // '['
                if (peek() == ']')
                {
                    get();
                    return v;
                }
                while (true)
                {
                    v.arr.push_back(parse_value());
                    char c = get();
                    if (c == ']')
                        break;
                    if (c != ',')
                        throw std::runtime_error("JSON: expected ',' or ']'");
                }
                return v;
            }

            std::string parse_string()
            {
                get(); // '"'
                std::string out;
                while (p < s.size() && s[p] != '"')
                {
                    if (s[p] == '\\' && p + 1 < s.size())
                    {
                        ++p;
                        char e = s[p++];
                        switch (e)
                        {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u':
                        {
                            if (p + 4 > s.size())
                                throw std::runtime_error("JSON: bad \\u escape");
                            uint32_t cp = 0;
                            for (int i = 0; i < 4; ++i)
                            {
                                int h = hex_val(s[p++]);
                                if (h < 0)
                                    throw std::runtime_error("JSON: bad \\u escape");
                                cp = (cp << 4) | (uint32_t)h;
                            }

                            if (cp >= 0xD800 && cp <= 0xDBFF &&
                                p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u')
                            {
                                p += 2;
                                uint32_t lo = 0;
                                for (int i = 0; i < 4; ++i)
                                {
                                    if (p >= s.size())
                                        throw std::runtime_error("JSON: bad surrogate");
                                    int h = hex_val(s[p++]);
                                    if (h < 0)
                                        throw std::runtime_error("JSON: bad surrogate");
                                    lo = (lo << 4) | (uint32_t)h;
                                }
                                if (lo >= 0xDC00 && lo <= 0xDFFF)
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            append_utf8(out, cp);
                            break;
                        }
                        default:
                            throw std::runtime_error(std::string("JSON: bad escape '\\") + e + "'");
                        }
                    }
                    else
                        out += s[p++];
                }
                get(); // '"'
                return out;
            }

            double parse_number(bool &is_int, int64_t &int_val)
            {
                size_t start = p;
                bool floating = false;
                while (p < s.size() && (std::isdigit((unsigned char)s[p]) ||
                                        s[p] == '-' || s[p] == '+' || s[p] == '.' || s[p] == 'e' || s[p] == 'E'))
                {
                    if (s[p] == '.' || s[p] == 'e' || s[p] == 'E')
                        floating = true;
                    ++p;
                }
                std::string numstr = s.substr(start, p - start);
                if (floating)
                {
                    is_int = false;
                    return std::strtod(numstr.c_str(), nullptr);
                }
                is_int = true;
                int_val = std::strtoll(numstr.c_str(), nullptr, 10);
                return (double)int_val;
            }

            void expect(const char *lit)
            {
                for (const char *q = lit; *q; ++q)
                    if (get() != *q)
                        throw std::runtime_error("JSON: unexpected token");
            }
        };

        inline Value parse(const std::string &s) { return Parser(s).parse_value(); }
    }
}