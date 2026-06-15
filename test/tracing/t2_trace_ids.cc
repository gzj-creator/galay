#include "galay-tracing/common/span_id.h"
#include "galay-tracing/common/trace_id.h"

#include <array>
#include <cassert>
#include <string>
#include <string_view>

namespace {

void traceIdFromHexRoundTripsLowercase() {
    constexpr std::string_view kHex = "0123456789abcdef0011223344556677";

    auto id = galay::tracing::TraceId::fromHex(kHex);

    assert(id.isValid());
    assert(id.toHex() == kHex);
}

void traceIdFromHexRejectsInvalidInput() {
    assert(!galay::tracing::TraceId::fromHex("0123456789abcdef001122334455667").isValid());
    assert(!galay::tracing::TraceId::fromHex("0123456789abcdef00112233445566770").isValid());
    assert(!galay::tracing::TraceId::fromHex("0123456789abcdef001122334455667g").isValid());
    assert(!galay::tracing::TraceId::fromHex("00000000000000000000000000000000").isValid());
}

void traceIdUppercaseInputNormalizesToLowercase() {
    auto id = galay::tracing::TraceId::fromHex("0123456789ABCDEF0011223344556677");

    assert(id.isValid());
    assert(id.toHex() == "0123456789abcdef0011223344556677");
}

void traceIdBufferFormattingAndEqualityWork() {
    auto id = galay::tracing::TraceId::fromHex("0123456789abcdef0011223344556677");
    auto same = galay::tracing::TraceId::fromHex("0123456789abcdef0011223344556677");
    std::array<char, galay::tracing::TraceId::kHexLength> chars{};
    char tooSmall[galay::tracing::TraceId::kHexLength - 1]{};

    assert(id == same);
    assert(id.toHex(chars.data(), chars.size()));
    assert(std::string(chars.data(), chars.size()) == "0123456789abcdef0011223344556677");
    assert(!id.toHex(tooSmall, sizeof(tooSmall)));
}

void spanIdFromHexRoundTripsLowercase() {
    constexpr std::string_view kHex = "0123456789abcdef";

    auto id = galay::tracing::SpanId::fromHex(kHex);

    assert(id.isValid());
    assert(id.toHex() == kHex);
}

void spanIdFromHexRejectsInvalidInput() {
    assert(!galay::tracing::SpanId::fromHex("0123456789abcde").isValid());
    assert(!galay::tracing::SpanId::fromHex("0123456789abcdef0").isValid());
    assert(!galay::tracing::SpanId::fromHex("0123456789abcdeg").isValid());
    assert(!galay::tracing::SpanId::fromHex("0000000000000000").isValid());
}

void spanIdUppercaseInputNormalizesToLowercase() {
    auto id = galay::tracing::SpanId::fromHex("0123456789ABCDEF");

    assert(id.isValid());
    assert(id.toHex() == "0123456789abcdef");
}

void spanIdBufferFormattingAndEqualityWork() {
    auto id = galay::tracing::SpanId::fromHex("0123456789abcdef");
    auto same = galay::tracing::SpanId::fromHex("0123456789abcdef");
    std::array<char, galay::tracing::SpanId::kHexLength> chars{};
    char tooSmall[galay::tracing::SpanId::kHexLength - 1]{};

    assert(id == same);
    assert(id.toHex(chars.data(), chars.size()));
    assert(std::string(chars.data(), chars.size()) == "0123456789abcdef");
    assert(!id.toHex(tooSmall, sizeof(tooSmall)));
}

void randomIdsAreNotZero() {
    assert(galay::tracing::TraceId::random().isValid());
    assert(galay::tracing::SpanId::random().isValid());
}

} // namespace

int main() {
    traceIdFromHexRoundTripsLowercase();
    traceIdFromHexRejectsInvalidInput();
    traceIdUppercaseInputNormalizesToLowercase();
    traceIdBufferFormattingAndEqualityWork();
    spanIdFromHexRoundTripsLowercase();
    spanIdFromHexRejectsInvalidInput();
    spanIdUppercaseInputNormalizesToLowercase();
    spanIdBufferFormattingAndEqualityWork();
    randomIdsAreNotZero();
}
