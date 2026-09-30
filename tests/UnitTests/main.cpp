// Runs every TEST_CASE; the exit code is the number of failed tests (0 = all passed).

#include "test.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>

namespace {

std::vector<std::string>* currentFailures = nullptr;

} // namespace

namespace test {

std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

void Fail(const char* file, int line, const std::string& message) {
    const char* name = strrchr(file, '\\');
    currentFailures->push_back(std::string(name ? name + 1 : file) + "(" + std::to_string(line) + "): " + message);
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace test

int main() {
    SetConsoleOutputCP(CP_UTF8);
    int failed = 0;
    for (const test::Case& testCase : test::Registry()) {
        std::vector<std::string> failures;
        currentFailures = &failures;
        try {
            testCase.run();
        } catch (const std::exception& error) {
            failures.push_back(std::string("exception: ") + error.what());
        } catch (...) {
            failures.push_back("unknown exception");
        }
        std::printf("%s %s\n", failures.empty() ? "[ OK ]" : "[FAIL]", testCase.name);
        for (const std::string& failure : failures) {
            std::printf("       %s\n", failure.c_str());
        }
        failed += failures.empty() ? 0 : 1;
    }
    std::printf("\n%zu tests, %d failed\n", test::Registry().size(), failed);
    return failed;
}
