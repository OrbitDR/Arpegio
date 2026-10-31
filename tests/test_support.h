// Small assertion helpers. Each test registers itself and test_main.cpp runs them all.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& test_registry();
void test_failed();

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> body) { test_registry().push_back({name, std::move(body)}); }
};

#define TEST(name)                                         \
    static void test_##name();                             \
    static TestRegistrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            test_failed();                                              \
        }                                                               \
    } while (0)

#define CHECK_NEAR(a, b, tolerance)                                                                 \
    do {                                                                                            \
        const double a_ = (a);                                                                      \
        const double b_ = (b);                                                                      \
        if (std::abs(a_ - b_) > (tolerance)) {                                                      \
            std::printf("  FAILED %s:%d: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, #a, a_, b_); \
            test_failed();                                                                          \
        }                                                                                           \
    } while (0)
