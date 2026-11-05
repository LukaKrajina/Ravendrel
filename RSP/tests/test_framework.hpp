#pragma once
// ─────────────────────────────────────────────────────────────
// rvdlRSP 极简单元测试框架(无外部依赖)
//
// 生产化增强:
//   - 每个测试独立 try/catch,单个断言失败或异常不中断后续测试
//   - 可选 JUnit XML 报告(通过环境变量 QTEST_JUNIT_OUTPUT 指定路径)
//   - 汇总统计(通过 / 失败 / 异常)+ 非零退出码
// ─────────────────────────────────────────────────────────────
#include <vector>
#include <string>
#include <functional>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace qtest {

    struct TestCase {
        std::string name;
        std::function<void()> fn;
    };

    // 单个测试的运行结果
    struct TestResult {
        std::string name;
        bool passed = false;
        std::string error;                // 异常信息(若有)
        std::vector<std::string> failures; // 断言失败详情
    };

    inline std::vector<TestCase> &registry() {
        static std::vector<TestCase> r;
        return r;
    }

    inline std::vector<TestResult> &results() {
        static std::vector<TestResult> r;
        return r;
    }

    // 全局断言失败计数(保留旧函数名,test_jpeg_decoder.cpp 等直接引用)
    inline int &failures() {
        static int f = 0;
        return f;
    }

    // 当前正在运行的测试结果指针(QCHECK 宏据此写入失败详情)
    inline TestResult *&current() {
        static TestResult *c = nullptr;
        return c;
    }

    struct Registrar {
        Registrar(const std::string &name, std::function<void()> fn) {
            registry().push_back({name, std::move(fn)});
        }
    };

    // ─── XML 特殊字符转义 ──────────────────────────────────
    inline std::string xml_escape(const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
            }
        }
        return out;
    }

    // 写 JUnit XML 报告(供 CI 解析)
    inline void write_junit(const std::string &path) {
        std::ofstream ofs(path);
        if (!ofs)
            return;
        int total = static_cast<int>(results().size());
        int failed = 0;
        for (const auto &r : results())
            if (!r.passed)
                ++failed;
        ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        ofs << "<testsuites tests=\"" << total << "\" failures=\"" << failed << "\">\n";
        ofs << "  <testsuite name=\"rvdlRSP_tests\" tests=\"" << total
            << "\" failures=\"" << failed << "\">\n";
        for (const auto &r : results()) {
            ofs << "    <testcase name=\"" << xml_escape(r.name) << "\"";
            if (r.passed) {
                ofs << " />\n";
            } else {
                ofs << ">\n";
                for (const auto &f : r.failures)
                    ofs << "      <failure message=\"" << xml_escape(f) << "\" />\n";
                if (!r.error.empty())
                    ofs << "      <failure message=\"exception: " << xml_escape(r.error) << "\" />\n";
                ofs << "    </testcase>\n";
            }
        }
        ofs << "  </testsuite>\n";
        ofs << "</testsuites>\n";
    }

    // ─── 运行所有测试(带异常隔离)──────────────────────────
    inline int run_all() {
        int passed = 0;
        for (const auto &t : registry()) {
            std::cerr << "[TEST] " << t.name << "\n";

            TestResult res;
            res.name = t.name;
            res.passed = true;
            current() = &res;
            int before = failures();

            try {
                t.fn();
            } catch (const std::exception &e) {
                res.passed = false;
                res.error = e.what();
                std::cerr << "  EXCEPTION: " << e.what() << "\n";
            } catch (...) {
                res.passed = false;
                res.error = "unknown exception";
                std::cerr << "  EXCEPTION: <unknown>\n";
            }

            current() = nullptr;
            if (failures() > before)
                res.passed = false;
            if (res.passed)
                ++passed;
            results().push_back(std::move(res));
        }

        int total = static_cast<int>(results().size());
        int failed = total - passed;

        std::cerr << "\n=== rvdlRSP test summary ===\n";
        std::cerr << "  passed: " << passed << "/" << total << "\n";
        if (failed > 0) {
            std::cerr << "  FAILED tests:\n";
            for (const auto &r : results()) {
                if (r.passed)
                    continue;
                std::cerr << "    - " << r.name;
                if (!r.error.empty())
                    std::cerr << "  [" << r.error << "]";
                else if (!r.failures.empty())
                    std::cerr << "  [" << r.failures.size() << " assertion(s)]";
                std::cerr << "\n";
            }
        }

        // 可选 JUnit XML 输出
        if (const char *p = std::getenv("QTEST_JUNIT_OUTPUT")) {
            write_junit(p);
            std::cerr << "  JUnit XML written to " << p << "\n";
        }

        std::cerr << (failed == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << "\n";
        return failed == 0 ? 0 : 1;
    }

}

#define QTEST(name) \
    static void qtest_##name(); \
    static ::qtest::Registrar qtest_reg_##name(#name, qtest_##name); \
    static void qtest_##name()

#define QCHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << "  @ " << __FILE__ << ":" << __LINE__ << "\n"; \
        ++::qtest::failures(); \
        if (::qtest::current()) \
            ::qtest::current()->failures.push_back(std::string(#cond) + " @ " + __FILE__ + ":" + std::to_string(__LINE__)); \
    } \
} while (0)

#define QCHECK_NEAR(a, b, eps) do { \
    double _a = (a), _b = (b); \
    if (std::fabs(_a - _b) > (eps)) { \
        std::cerr << "  FAIL: " << #a << " (" << _a << ") != " << #b << " (" << _b << ")  @ " \
                  << __FILE__ << ":" << __LINE__ << "\n"; \
        ++::qtest::failures(); \
        if (::qtest::current()) \
            ::qtest::current()->failures.push_back(std::string(#a) + " != " + #b + " @ " + __FILE__ + ":" + std::to_string(__LINE__)); \
    } \
} while (0)