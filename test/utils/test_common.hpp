#pragma once

#include <iostream>
#include <cassert>
#include <array>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstring>
#include <functional>
#include <iomanip>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>
#include <unordered_map>

#include "utils/galay_utils.hpp"
#include <utils/tool/rate_limiter.hpp>

using namespace galay::utils;

struct ManualClock {
    using rep = int64_t;
    using period = std::milli;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<ManualClock>;

    static constexpr bool is_steady = true;
    static inline time_point current{duration{0}};

    static time_point now() noexcept {
        return current;
    }

    static void reset() {
        current = time_point{duration{0}};
    }

    static void advance(duration delta) {
        current += delta;
    }
};

struct CountingClock {
    using rep = int64_t;
    using period = std::milli;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<CountingClock>;

    static constexpr bool is_steady = true;
    static inline int nowCalls = 0;
    static inline time_point current{duration{0}};

    static time_point now() noexcept {
        ++nowCalls;
        return current;
    }

    static void reset() {
        nowCalls = 0;
        current = time_point{duration{0}};
    }
};

struct CaseInsensitiveHash {
    size_t operator()(const std::string& value) const {
        size_t hash = 0;
        for (unsigned char ch : value) {
            hash = hash * 131 + static_cast<unsigned char>(std::tolower(ch));
        }
        return hash;
    }
};

struct CaseInsensitiveEqual {
    bool operator()(const std::string& lhs, const std::string& rhs) const {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                std::tolower(static_cast<unsigned char>(rhs[i]))) {
                return false;
            }
        }
        return true;
    }
};

// ==================== String Tests ====================
