#pragma once

// A minimal test harness: TEST_CASE registers a function, CHECK / CHECK_EQ / CHECK_CONTAINS record
// failures without stopping the test. main.cpp runs everything and returns the number of failed tests.

#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*run)();
};

std::vector<Case>& Registry();
void Fail(const char* file, int line, const std::string& message);
std::string Narrow(const std::wstring& text);  // UTF-8

struct Register {
    Register(const char* name, void (*run)()) { Registry().push_back({ name, run }); }
};

template <typename T>
std::string Show(const T& value) {
    std::ostringstream output;
    output << value;
    return output.str();
}
inline std::string Show(const std::wstring& value) { return "\"" + Narrow(value) + "\""; }
inline std::string Show(const wchar_t* value) { return Show(std::wstring(value)); }
template <size_t N>
std::string Show(const wchar_t (&value)[N]) { return Show(std::wstring(value)); }
inline std::string Show(bool value) { return value ? "true" : "false"; }

} // namespace test

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static const test::Register name##_registration(#name, &name);            \
    static void name()

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            test::Fail(__FILE__, __LINE__, "CHECK(" #condition ") failed");            \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        const auto& actual_ = (actual);                                                         \
        const auto& expected_ = (expected);                                                     \
        if (!(actual_ == expected_)) {                                                          \
            test::Fail(__FILE__, __LINE__, std::string(#actual " == " #expected ": got ") +    \
                                               test::Show(actual_) + ", expected " + test::Show(expected_)); \
        }                                                                                       \
    } while (0)

#define CHECK_CONTAINS(text, part)                                                                        \
    do {                                                                                                  \
        const std::wstring text_ = (text);                                                                \
        const std::wstring part_ = (part);                                                                \
        if (text_.find(part_) == std::wstring::npos) {                                                    \
            test::Fail(__FILE__, __LINE__, std::string(#text " does not contain ") + test::Show(part_) + \
                                               "; text was:\n" + test::Narrow(text_));                   \
        }                                                                                                 \
    } while (0)
