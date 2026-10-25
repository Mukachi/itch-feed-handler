// The tests' one assertion, as in the gateway: each check prints pass or FAIL
// with what it checked; a test exits non-zero if any failed.
#pragma once

#include <cstdio>
#include <string>

namespace itchfh::test {

inline int failures = 0;

inline void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

inline int result() {
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

}  // namespace itchfh::test
