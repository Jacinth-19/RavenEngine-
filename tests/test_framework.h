// RavenEngine — minimal test framework (no external dependencies).
// Tests register via RAVEN_TEST("name") { ... }; CHECK aborts the current test on failure.
#pragma once

#include <cstdio>
#include <string>
#include <vector>
#include <exception>

namespace raven::test {

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, void (*fn)()) { Registry().push_back({std::move(name), fn}); }
};

class Failure : public std::exception {
public:
    explicit Failure(std::string message) : m_Message(std::move(message)) {}
    const char* what() const noexcept override { return m_Message.c_str(); }
private:
    std::string m_Message;
};

[[noreturn]] inline void Fail(const char* expression, const char* file, int line, const std::string& detail = {}) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf), "%s:%d: check failed: %s%s%s", file, line, expression,
                  detail.empty() ? "" : " — ", detail.c_str());
    throw Failure(buf);
}

inline int RunAll() {
    int passed = 0, failed = 0;
    for (const auto& test : Registry()) {
        try {
            test.fn();
            ++passed;
            std::fprintf(stderr, "[ PASS ] %s\n", test.name.c_str());
        } catch (const Failure& f) {
            ++failed;
            std::fprintf(stderr, "[ FAIL ] %s\n        %s\n", test.name.c_str(), f.what());
        } catch (const std::exception& e) {
            ++failed;
            std::fprintf(stderr, "[ FAIL ] %s\n        unexpected exception: %s\n", test.name.c_str(), e.what());
        } catch (...) {
            ++failed;
            std::fprintf(stderr, "[ FAIL ] %s\n        unknown exception\n", test.name.c_str());
        }
    }
    std::fprintf(stderr, "\n%d passed, %d failed, %zu total\n", passed, failed, Registry().size());
    return failed == 0 ? 0 : 1;
}

} // namespace raven::test

#define RAVEN_TEST_CONCAT_I(a, b) a##b
#define RAVEN_TEST_CONCAT(a, b) RAVEN_TEST_CONCAT_I(a, b)

#define RAVEN_TEST(name)                                                       \
    static void RAVEN_TEST_CONCAT(raven_test_fn_, __LINE__)();                  \
    static ::raven::test::Registrar RAVEN_TEST_CONCAT(raven_test_reg_, __LINE__)(name, &RAVEN_TEST_CONCAT(raven_test_fn_, __LINE__)); \
    static void RAVEN_TEST_CONCAT(raven_test_fn_, __LINE__)()

#define CHECK(cond)                                                            \
    do { if (!(cond)) ::raven::test::Fail(#cond, __FILE__, __LINE__); }        \
    while (0)

#define CHECK_EQ(a, b)                                                         \
    do { auto&& _va = (a); auto&& _vb = (b);                                   \
         if (!(_va == _vb)) {                                                  \
             char _b[160];                                                     \
             std::snprintf(_b, sizeof(_b), "lhs=%lld rhs=%lld",                 \
                           (long long)_va, (long long)_vb);                    \
             ::raven::test::Fail(#a " == " #b, __FILE__, __LINE__, _b);        \
         } } while (0)

#define REQUIRE(cond) CHECK(cond)
