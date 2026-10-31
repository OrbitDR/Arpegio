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

int main() {
    for (const TestCase& test : test_registry()) {
        std::printf("%s\n", test.name);
        test.body();
    }
    std::printf("%zu tests, %d failures\n", test_registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
