#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/uio.h>

#include <galay-utils/cache/ring_buffer.hpp>

int main()
{
    galay::utils::RingBuffer buffer(8);

    std::array<iovec, 2> write_iovecs{};
    size_t count = buffer.getWriteIovecs(write_iovecs);
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
