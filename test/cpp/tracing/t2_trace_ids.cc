#include <galay/cpp/galay-tracing/common/span_id.h>
#include <galay/cpp/galay-tracing/common/trace_id.h>

#include <array>
#include <cassert>
#include <string>
#include <string_view>

namespace {

void trace_id_from_hex_round_trips_lowercase() {
    constexpr std::string_view kHex = "0123456789abcdef0011223344556677";

    auto id = galay::tracing::TraceId::from_hex(kHex);

    assert(id.is_valid());
    assert(id.to_hex() == kHex);
}

void trace_id_from_hex_rejects_invalid_input() {
    assert(!galay::tracing::TraceId::from_hex("0123456789abcdef001122334455667").is_valid());
    assert(!galay::tracing::TraceId::from_hex("0123456789abcdef00112233445566770").is_valid());
    assert(!galay::tracing::TraceId::from_hex("0123456789abcdef001122334455667g").is_valid());
    assert(!galay::tracing::TraceId::from_hex("00000000000000000000000000000000").is_valid());
}

void trace_id_uppercase_input_normalizes_to_lowercase() {
    auto id = galay::tracing::TraceId::from_hex("0123456789ABCDEF0011223344556677");

    assert(id.is_valid());
    assert(id.to_hex() == "0123456789abcdef0011223344556677");
}

void trace_id_buffer_formatting_and_equality_work() {
    auto id = galay::tracing::TraceId::from_hex("0123456789abcdef0011223344556677");
    auto same = galay::tracing::TraceId::from_hex("0123456789abcdef0011223344556677");
    std::array<char, galay::tracing::TraceId::kHexLength> chars{};
    char tooSmall[galay::tracing::TraceId::kHexLength - 1]{};

    assert(id == same);
    assert(id.to_hex(chars.data(), chars.size()));
    assert(std::string(chars.data(), chars.size()) == "0123456789abcdef0011223344556677");
    assert(!id.to_hex(tooSmall, sizeof(tooSmall)));
}

void span_id_from_hex_round_trips_lowercase() {
    constexpr std::string_view kHex = "0123456789abcdef";

    auto id = galay::tracing::SpanId::from_hex(kHex);

    assert(id.is_valid());
    assert(id.to_hex() == kHex);
}

void span_id_from_hex_rejects_invalid_input() {
    assert(!galay::tracing::SpanId::from_hex("0123456789abcde").is_valid());
    assert(!galay::tracing::SpanId::from_hex("0123456789abcdef0").is_valid());
    assert(!galay::tracing::SpanId::from_hex("0123456789abcdeg").is_valid());
    assert(!galay::tracing::SpanId::from_hex("0000000000000000").is_valid());
}

void span_id_uppercase_input_normalizes_to_lowercase() {
    auto id = galay::tracing::SpanId::from_hex("0123456789ABCDEF");

    assert(id.is_valid());
    assert(id.to_hex() == "0123456789abcdef");
}

void span_id_buffer_formatting_and_equality_work() {
    auto id = galay::tracing::SpanId::from_hex("0123456789abcdef");
    auto same = galay::tracing::SpanId::from_hex("0123456789abcdef");
    std::array<char, galay::tracing::SpanId::kHexLength> chars{};
    char tooSmall[galay::tracing::SpanId::kHexLength - 1]{};

    assert(id == same);
    assert(id.to_hex(chars.data(), chars.size()));
    assert(std::string(chars.data(), chars.size()) == "0123456789abcdef");
    assert(!id.to_hex(tooSmall, sizeof(tooSmall)));
}

void random_ids_are_not_zero() {
    assert(galay::tracing::TraceId::random().is_valid());
    assert(galay::tracing::SpanId::random().is_valid());
}

} // namespace

int main() {
    trace_id_from_hex_round_trips_lowercase();
    trace_id_from_hex_rejects_invalid_input();
    trace_id_uppercase_input_normalizes_to_lowercase();
    trace_id_buffer_formatting_and_equality_work();
    span_id_from_hex_round_trips_lowercase();
    span_id_from_hex_rejects_invalid_input();
    span_id_uppercase_input_normalizes_to_lowercase();
    span_id_buffer_formatting_and_equality_work();
    random_ids_are_not_zero();
}
