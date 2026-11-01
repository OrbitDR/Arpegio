#include <exception>
#include <string_view>

#include "test_support.h"

namespace {
int failures = 0;
}

std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void test_failed() {
    ++failures;
}

// An argument runs only the tests with that text in their name.
int main(int count, char** arguments) {
    const std::string_view wanted = count > 1 ? arguments[1] : "";
    for (const TestCase& test : test_registry()) {
        if (std::string_view(test.name).find(wanted) == std::string_view::npos) continue;
        std::printf("%s\n", test.name);
        std::fflush(stdout);  // what was running must still show if the process dies
        try {
            test.body();
        } catch (const std::exception& error) {
            std::printf("  FAILED: exception: %s\n", error.what());
            test_failed();
        }
    }
    std::printf("%zu tests, %d failures\n", test_registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
