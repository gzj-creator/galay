/**
 * @file b16_h2_move_clone_pressure.cc
 * @brief HTTP/2 move-only ownership and explicit clone pressure benchmark.
 */

#include <galay/cpp/galay-http2/kernel/http2_stream.h>
#include <galay/cpp/galay-http2/protoc/http2_frame.h>
#include <galay/cpp/galay-http2/protoc/http2_hpack.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::http2;

namespace {

struct BenchResult {
    double ns_per_op = 0.0;
    size_t checksum = 0;
};

template<typename Fn>
BenchResult runTimed(size_t iterations, Fn&& fn)
{
    size_t checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        checksum += fn(i);
    }
    auto stop = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    const double ns_per_op = iterations == 0
        ? 0.0
        : static_cast<double>(elapsed_ns) / static_cast<double>(iterations);
    return BenchResult{.ns_per_op = ns_per_op, .checksum = checksum};
}

Http2HeadersFrame makeHeadersFrame()
{
    Http2HeadersFrame frame;
    frame.header().stream_id = 1;
    frame.setEndHeaders(true);
    frame.setHeaderBlock(std::string(512, 'h'));
    return frame;
}

Http2ChunkedBody makeBody()
{
    Http2ChunkedBody body;
    body.append(std::string(256, 'a'));
    body.append(std::string(512, 'b'));
    body.append(std::string(1024, 'c'));
    return body;
}

HpackDynamicTable makeDynamicTable()
{
    HpackDynamicTable table;
    table.add({"x-alpha", std::string(64, 'a')});
    table.add({"x-beta", std::string(128, 'b')});
    table.add({"x-gamma", std::string(256, 'c')});
    return table;
}

HpackEncoder makeEncoder()
{
    HpackEncoder encoder;
    encoder.dynamicTable().add({"x-alpha", std::string(64, 'a')});
    encoder.dynamicTable().add({"x-beta", std::string(128, 'b')});
    encoder.setMaxTableSize(1024);
    return encoder;
}

HpackDecoder makeDecoder()
{
    HpackDecoder decoder;
    decoder.dynamicTable().add({"x-alpha", std::string(64, 'a')});
    decoder.dynamicTable().add({"x-beta", std::string(128, 'b')});
    decoder.setMaxHeaderListSize(4096);
    return decoder;
}

} // namespace

int main(int argc, char* argv[])
{
    size_t iterations = 100000;
    if (argc > 1) {
        const int parsed = std::atoi(argv[1]);
        if (parsed > 0) {
            iterations = static_cast<size_t>(parsed);
        }
    }

    const auto frame_seed = makeHeadersFrame();
    const auto frame_clone = runTimed(iterations, [&](size_t i) {
        auto cloned = frame_seed.clone();
        return cloned.headerBlock().size() + cloned.streamId() + i % 7;
    });

    const auto body_seed = makeBody();
    const auto body_clone_move = runTimed(iterations, [&](size_t i) {
        auto cloned = body_seed.clone();
        Http2ChunkedBody moved = std::move(cloned);
        return moved.size() + moved.chunkCount() + i % 5;
    });

    const auto table_seed = makeDynamicTable();
    const auto encoder_seed = makeEncoder();
    const auto decoder_seed = makeDecoder();
    const auto hpack_clone_construct = runTimed(iterations, [&](size_t i) {
        auto table_copy = table_seed.clone();
        auto encoder_copy = encoder_seed.clone();
        auto decoder_copy = decoder_seed.clone();
        HpackDynamicTable fresh_table;
        HpackEncoder fresh_encoder;
        HpackDecoder fresh_decoder;
        return table_copy.count()
            + encoder_copy.dynamicTable().count()
            + decoder_copy.dynamicTable().count()
            + fresh_table.count()
            + fresh_encoder.dynamicTable().count()
            + fresh_decoder.dynamicTable().count()
            + i % 3;
    });

    const size_t checksum = frame_clone.checksum
        + body_clone_move.checksum
        + hpack_clone_construct.checksum;
    if (checksum == 0) {
        std::cerr << "benchmark checksum must not be zero\n";
        return 1;
    }

    std::cout << "HTTP/2 move/clone pressure benchmark\n";
    std::cout << "iterations=" << iterations << "\n";
    std::cout << "frame_clone_ns=" << static_cast<size_t>(frame_clone.ns_per_op)
              << " checksum=" << frame_clone.checksum << "\n";
    std::cout << "body_clone_move_ns=" << static_cast<size_t>(body_clone_move.ns_per_op)
              << " checksum=" << body_clone_move.checksum << "\n";
    std::cout << "hpack_clone_construct_ns="
              << static_cast<size_t>(hpack_clone_construct.ns_per_op)
              << " checksum=" << hpack_clone_construct.checksum << "\n";
    return 0;
}
