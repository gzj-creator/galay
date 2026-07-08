#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/uio.h>
#include <vector>

#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

static_assert(galay::utils::RingBufferBackendStrategy::Mmap != galay::utils::RingBufferBackendStrategy::Vector);
static_assert(galay::utils::RingBufferBackendStrategy::Auto != galay::utils::RingBufferBackendStrategy::Mmap);

int main()
{
    using galay::utils::RingBufferBackendStrategy;

    galay::utils::RingBuffer<> default_buffer(8);
    const size_t default_capacity = default_buffer.capacity();

    std::vector<char> prefix(default_capacity - 2, 'x');
    if (default_buffer.write(prefix.data(), prefix.size()) != prefix.size()) {
        std::cerr << "failed to prepare default mmap prefix\n";
        return 1;
    }
    default_buffer.consume(default_capacity - 4);
    if (default_buffer.write("abcdef", 6) != 6) {
        std::cerr << "failed to prepare default mmap wrapped payload\n";
        return 1;
    }

    std::array<iovec, 2> default_read_iovecs{};
    size_t count = default_buffer.getReadIovecs(default_read_iovecs);
    if (count != 1 || default_read_iovecs[0].iov_len != default_buffer.readable()) {
        std::cerr << "default mmap strategy must expose wrapped readable data as one iovec\n";
        return 1;
    }

    galay::utils::RingBuffer<RingBufferBackendStrategy::Vector> buffer(8);

    std::array<iovec, 2> write_iovecs{};
    count = buffer.getWriteIovecs(write_iovecs);
    if (count != 1 || write_iovecs[0].iov_len != 8) {
        std::cerr << "unexpected initial write iovec layout\n";
        return 1;
    }

    std::memcpy(write_iovecs[0].iov_base, "abcdef", 6);
    buffer.produce(6);

    std::array<iovec, 2> read_iovecs{};
    count = buffer.getReadIovecs(read_iovecs);
    if (count != 1 || read_iovecs[0].iov_len != 6) {
        std::cerr << "unexpected first read iovec layout\n";
        return 1;
    }

    std::string first(static_cast<char*>(read_iovecs[0].iov_base), read_iovecs[0].iov_len);
    if (first != "abcdef") {
        std::cerr << "unexpected first payload: " << first << '\n';
        return 1;
    }

    buffer.consume(5);

    count = buffer.getWriteIovecs(write_iovecs);
    if (count != 2) {
        std::cerr << "expected wrapped write iovecs\n";
        return 1;
    }

    std::memcpy(write_iovecs[0].iov_base, "gh", 2);
    std::memcpy(write_iovecs[1].iov_base, "ij", 2);
    buffer.produce(4);

    count = buffer.getReadIovecs(read_iovecs);
    if (count != 2) {
        std::cerr << "expected wrapped read iovecs\n";
        return 1;
    }

    std::string wrapped;
    wrapped.append(static_cast<char*>(read_iovecs[0].iov_base), read_iovecs[0].iov_len);
    wrapped.append(static_cast<char*>(read_iovecs[1].iov_base), read_iovecs[1].iov_len);
    if (wrapped != "fghij") {
        std::cerr << "unexpected wrapped payload: " << wrapped << '\n';
        return 1;
    }

    buffer.clear();
    if (!buffer.empty() || buffer.readable() != 0 || buffer.writable() != buffer.capacity()) {
        std::cerr << "clear did not reset buffer state\n";
        return 1;
    }

    return 0;
}
