#pragma once

#include <cstdio>

#define BLIP_CHECK(condition)                                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

struct TestCase {
    const char* name;
    bool (*run)();
};

template <std::size_t Count> int run_tests(const TestCase (&tests)[Count]) {
    std::size_t passed = 0;
    for (const auto& test : tests) {
        if (!test.run()) {
            std::fprintf(stderr, "FAILED %s\n", test.name);
            return 1;
        }
        ++passed;
        std::printf("PASS %s\n", test.name);
    }
    std::printf("Host suite passed (%zu tests)\n", passed);
    return 0;
}
