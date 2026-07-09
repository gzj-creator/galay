#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/uio.h>
#endif

namespace {

#if defined(__unix__) || defined(__APPLE__)

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::abort();
    }
}

void test_mmap_error_surface() {
    using galay::utils::RingBufferError;

    const RingBufferError errors[] = {
        RingBufferError::kOk,
        RingBufferError::kInvalidCapacity,
        RingBufferError::kSharedMemoryCreateFail,
        RingBufferError::kResizeFail,
        RingBufferError::kAddressReserveFail,
        RingBufferError::kMappingFail,
    };
    for (RingBufferError error : errors) {
        require(galay::utils::ringBufferErrorString(error)[0] != '\0',
                "RingBufferError string must be non-empty");
    }

    auto invalid = galay::utils::detail::MmapRingBufferImpl::create(0);
    require(!invalid, "mmap ring creation with zero capacity must fail");
    require(invalid.error() == RingBufferError::kInvalidCapacity,
            "zero capacity must report kInvalidCapacity");

    auto invalid_ring = galay::utils::RingBuffer<>::create(0);
    require(!invalid_ring, "public ring creation with zero capacity must fail without throwing");
    require(invalid_ring.error() == RingBufferError::kInvalidCapacity,
            "public zero capacity creation must report kInvalidCapacity");

    auto valid_ring = galay::utils::RingBuffer<>::create(128);
    require(valid_ring.has_value(), "public ring creation with positive capacity must succeed");
    require(valid_ring->capacity() >= 128, "public ring creation must preserve requested capacity");
}

void test_public_constructor_zero_capacity_falls_back() {
    galay::utils::RingBuffer<> buffer(0);
    require(buffer.capacity() >= galay::utils::RingBuffer<>::kDefaultCapacity,
            "zero-capacity constructor must fall back to a usable default capacity");
    constexpr std::string_view data = "x";
    require(buffer.write(data) == data.size(),
            "zero-capacity constructor fallback buffer must accept writes");
}

void test_threshold_buffer_uses_single_iovec_across_wrap() {
    galay::utils::RingBuffer<> buffer(galay::utils::RingBuffer<>::kMmapThreshold);
    const std::size_t capacity = buffer.capacity();
    require(capacity >= galay::utils::RingBuffer<>::kMmapThreshold,
            "mmap ring capacity must cover requested capacity");

    std::vector<char> prefix(capacity - 64, 'A');
    require(buffer.write(prefix.data(), prefix.size()) == prefix.size(),
            "initial write must fill prefix");
    buffer.consume(capacity - 128);
    require(buffer.readable() == 64, "setup must leave tail bytes readable");

    std::array<struct iovec, 2> write_iovecs{};
    const std::size_t write_count = buffer.getWriteIovecs(write_iovecs);
    require(write_count == 1, "mmap wrapped writable area must be a single iovec");
    require(write_iovecs[0].iov_len == buffer.writable(),
            "single write iovec must cover all writable bytes");

    std::vector<char> wrapped(256, 'B');
    require(buffer.write(wrapped.data(), wrapped.size()) == wrapped.size(),
            "wrapped write must complete");

    std::array<struct iovec, 2> read_iovecs{};
    const std::size_t read_count = buffer.getReadIovecs(read_iovecs);
    require(read_count == 1, "mmap wrapped readable area must be a single iovec");
    require(read_iovecs[0].iov_len == 320,
            "single read iovec must cover tail and head bytes");

    const auto* data = static_cast<const char*>(read_iovecs[0].iov_base);
    require(std::memcmp(data, std::vector<char>(64, 'A').data(), 64) == 0,
            "tail bytes must remain readable through contiguous view");
    require(std::memcmp(data + 64, wrapped.data(), wrapped.size()) == 0,
            "wrapped bytes must follow tail bytes in contiguous view");
}

#endif

} // namespace

int main() {
#if defined(__unix__) || defined(__APPLE__)
    test_mmap_error_surface();
    test_public_constructor_zero_capacity_falls_back();
    test_threshold_buffer_uses_single_iovec_across_wrap();
#endif
    return 0;
}
