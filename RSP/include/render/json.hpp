#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <cstdlib>
#include <stdexcept>

namespace rvdlrsp::json
{

    struct Value;
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Null = std::nullptr_t;

    struct Value
    {
        std::variant<Null, bool, double, std::string, Array, Object> data;

        Value() : data(nullptr) {}
        Value(bool b) : data(b) {}
        Value(double d) : data(d) {}
        Value(int i) : data(static_cast<double>(i)) {}
        Value(const std::string &s) : data(s) {}
        Value(const char *s) : data(std::string(s)) {}
        Value(Array a) : data(std::move(a)) {}
        Value(Object o) : data(std::move(o)) {}

        bool is_object() const { return std::holds_alternative<Object>(data); }
        bool is_array() const { return std::holds_alternative<Array>(data); }
        bool is_number() const { return std::holds_alternative<double>(data); }
        bool is_string() const { return std::holds_alternative<std::string>(data); }
        bool is_bool() const { return std::holds_alternative<bool>(data); }

        double number() const { return std::get<double>(data); }
        const std::string &string() const { return std::get<std::string>(data); }
        const Array &array() const { return std::get<Array>(data); }
        const Object &object() const { return std::get<Object>(data); }

        // 取对象成员
        const Value &at(const std::string &key) const
        {
            const Object &o = object();
            auto it = o.find(key);
            if (it == o.end())
                throw std::runtime_error("JSON: missing key '" + key + "'");
            return it->second;
        }

        // 取数组下标
        const Value &operator[](size_t i) const { return array()[i]; }
    };

    class Parser
    {
    private:
        const std::string &s_;
        size_t p_ = 0;

        void skip_ws()
        {
            while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r'))
                ++p_;
        }
        char peek()
        {
            skip_ws();
            return p_ < s_.size() ? s_[p_] : '\0';
        }
        char get()
        {
            skip_ws();
            return p_ < s_.size() ? s_[p_++] : '\0';
        }

        Value parse_value()
        {
            char c = peek();
            if (c == '{')
                return parse_object();
            if (c == '[')
                return parse_array();
            if (c == '"')
                return parse_string();
            if (c == 't')
            {
                expect("true");
                return Value(true);
            }
            if (c == 'f')
            {
                expect("false");
                return Value(false);
            }
            if (c == 'n')
            {
                expect("null");
                return Value();
            }
            return parse_number();
        }

        Value parse_object()
        {
            Object o;
            get(); // '{'
            if (peek() == '}')
            {
                get();
                return Value(o);
            }
            while (true)
            {
                std::string key = parse_string();
                if (get() != ':')
                    throw std::runtime_error("JSON: expected ':'");
                o[key] = parse_value();
                char c = get();
                if (c == '}')
                    break;
                if (c != ',')
                    throw std::runtime_error("JSON: expected ',' or '}'");
            }
            return Value(o);
        }

        Value parse_array()
        {
            Array a;
            get(); // '['
            if (peek() == ']')
            {
                get();
                return Value(a);
            }
            while (true)
            {
                a.push_back(parse_value());
                char c = get();
                if (c == ']')
                    break;
                if (c != ',')
                    throw std::runtime_error("JSON: expected ',' or ']'");
            }
            return Value(a);
        }

        std::string parse_string()
        {
            get(); // '"'
            std::string out;
            while (p_ < s_.size() && s_[p_] != '"')
            {
                if (s_[p_] == '\\' && p_ + 1 < s_.size())
                {
                    ++p_;
                    char e = s_[p_++];
                    switch (e)
                    {
                    case 'n':
                        out += '\n';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    default:
                        out += e;
                        break;
                    }
                }
                else
                {
                    out += s_[p_++];
                }
            }
            get(); // '"'
            return out;
        }

        double parse_number()
        {
            size_t start = p_;
            while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) ||
                                      s_[p_] == '-' || s_[p_] == '+' || s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E'))
                ++p_;
            return std::strtod(s_.substr(start, p_ - start).c_str(), nullptr);
        }

        void expect(const char *lit)
        {
            for (const char *q = lit; *q; ++q)
                if (get() != *q)
                    throw std::runtime_error("JSON: unexpected token");
        }

    public:
        explicit Parser(const std::string &s) : s_(s) {}
        static Value parse(const std::string &s)
        {
            Parser p(s);
            Value v = p.parse_value();
            return v;
        }
    };

    inline Value parse(const std::string &s) { return Parser::parse(s); }
}