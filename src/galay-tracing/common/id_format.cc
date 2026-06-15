/**
 * @file id_format.cc
 * @brief 追踪标识符的十六进制解析、格式化与随机生成实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现十六进制字符串与字节数组的双向转换，以及基于 SplitMix64
 * 伪随机数生成器的 TraceId / SpanId 随机生成。
 */

#include "galay-tracing/common/id_format.h"

#include "galay-tracing/common/span_id.h"
#include "galay-tracing/common/trace_id.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>

namespace galay::tracing::detail {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

std::uint64_t fallbackSeed() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<std::uint64_t>(now) ^ (counter.fetch_add(1, std::memory_order_relaxed) + 0x9e3779b97f4a7c15ULL);
}

std::uint64_t randomSeed() noexcept {
    try {
        std::random_device device;
        const auto high = static_cast<std::uint64_t>(device());
        const auto low = static_cast<std::uint64_t>(device());
        return (high << 32U) ^ low ^ fallbackSeed();
    } catch (...) {
        return fallbackSeed();
    }
}

std::uint64_t nextRandom64() noexcept {
    thread_local std::uint64_t state = randomSeed();
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

template <std::size_t N>
void fillRandomBytes(std::array<std::byte, N>& bytes) noexcept {
    if constexpr (N == sizeof(std::uint64_t)) {
        const auto chunk = nextRandom64();
        std::memcpy(bytes.data(), &chunk, sizeof(chunk));
    } else if constexpr (N == sizeof(std::uint64_t) * 2) {
        const std::array chunks{nextRandom64(), nextRandom64()};
        std::memcpy(bytes.data(), chunks.data(), sizeof(chunks));
    } else {
        std::uint64_t chunk = 0;
        int remaining = 0;

        for (auto& byte : bytes) {
            if (remaining == 0) {
                chunk = nextRandom64();
                remaining = 8;
            }
            byte = static_cast<std::byte>(chunk & 0xffU);
            chunk >>= 8U;
            --remaining;
        }
    }
}

} // namespace

bool parseHex(std::string_view hex, std::byte* out, std::size_t len) noexcept {
    if (out == nullptr || hex.size() != len * 2) {
        return false;
    }

    for (std::size_t i = 0; i < len; ++i) {
        const int high = hexValue(hex[i * 2]);
        const int low = hexValue(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

bool formatHex(const std::byte* bytes, std::size_t byteLen, char* out, std::size_t outLen) noexcept {
    if (bytes == nullptr || out == nullptr || outLen < byteLen * 2) {
        return false;
    }

    for (std::size_t i = 0; i < byteLen; ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        out[i * 2] = kHexDigits[value >> 4];
        out[i * 2 + 1] = kHexDigits[value & 0x0fU];
    }
    return true;
}

TraceId makeRandomTraceId() noexcept {
    TraceId::Bytes bytes{};
    do {
        fillRandomBytes(bytes);
    } while (!hasNonZeroByte(bytes.data(), bytes.size()));
    return TraceId(bytes);
}

SpanId makeRandomSpanId() noexcept {
    SpanId::Bytes bytes{};
    do {
        fillRandomBytes(bytes);
    } while (!hasNonZeroByte(bytes.data(), bytes.size()));
    return SpanId(bytes);
}

} // namespace galay::tracing::detail

namespace galay::tracing {

TraceId TraceId::random() noexcept {
    return detail::makeRandomTraceId();
}

SpanId SpanId::random() noexcept {
    return detail::makeRandomSpanId();
}

} // namespace galay::tracing
