#pragma once
// Included ONLY in an explicitly compiled test binary, never production builds.
#include <cstdint>
#include <cstdlib>
#include <windows.h>
namespace Stage4Faults {
    inline uint64_t Value(const char* key, uint64_t fallback) {
        char text[32]{};
        if (!GetEnvironmentVariableA(key, text, sizeof(text))) return fallback;
        return std::strtoull(text, nullptr, 10);
    }
    inline bool Fail(const char* key, uint64_t done) {
        return done >= Value(key, UINT64_MAX);
    }
}
