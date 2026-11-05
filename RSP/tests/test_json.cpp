// JSON 解析器单元测试
#include "test_framework.hpp"
#include "render/json.hpp"

using namespace rvdlrsp::json;

QTEST(json_parse_number) {
    Value v = parse("42.5");
    QCHECK(v.is_number());
    QCHECK_NEAR(v.number(), 42.5, 1e-9);
}

QTEST(json_parse_string) {
    Value v = parse("\"hello\"");
    QCHECK(v.is_string());
    QCHECK(v.string() == "hello");
}

QTEST(json_parse_bool_null) {
    QCHECK(parse("true").is_bool());
    QCHECK(parse("false").is_bool());
    QCHECK(!parse("null").is_object());
}

QTEST(json_parse_array) {
    Value v = parse("[1, 2, 3]");
    QCHECK(v.is_array());
    QCHECK(v.array().size() == 3);
    QCHECK_NEAR(v[0].number(), 1, 1e-9);
    QCHECK_NEAR(v[2].number(), 3, 1e-9);
}

QTEST(json_parse_object) {
    Value v = parse("{\"a\": 1, \"b\": \"x\"}");
    QCHECK(v.is_object());
    QCHECK_NEAR(v.at("a").number(), 1, 1e-9);
    QCHECK(v.at("b").string() == "x");
}

QTEST(json_parse_nested) {
    Value v = parse("{\"buffers\": [{\"uri\": \"data:;base64,AA==\", \"byteLength\": 1}]}");
    QCHECK(v.is_object());
    QCHECK(v.at("buffers").is_array());
    QCHECK(v.at("buffers")[0].at("uri").string() == "data:;base64,AA==");
}