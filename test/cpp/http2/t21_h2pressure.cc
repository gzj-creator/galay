/**
 * @file T85-H2KernelPressure.cc
 * @brief HTTP/2 kernel dispatcher/scheduler pressure regression test
 */

#include <galay/cpp/galay-http2/kernel/flow_control.h>
#include <galay/cpp/galay-http2/kernel/frame_dispacher.h>
#include <galay/cpp/galay-http2/kernel/out_scheduler.h>
#include <cassert>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace galay::http2;

namespace
{

void stress_many_streams_fairness()
{
    constexpr size_t kStreams = 1000;
    constexpr size_t kPayloadBytes = 128;
    constexpr size_t kFrameBytes = 16;

    std::vector<H2StreamSendState> streams;
    streams.reserve(kStreams);
    for (size_t i = 0; i < kStreams; ++i) {
        streams.push_back(H2StreamSendState{
            .pending = {
                .chunks = {std::string(kPayloadBytes, static_cast<char>('a' + (i % 26)))},
                .front_offset = 0,
                .end_stream = false
            },
            .stream_id = static_cast<uint32_t>(1 + i * 2),
            .stream_window = static_cast<int32_t>(kPayloadBytes),
            .weight = static_cast<uint8_t>((i % 2) == 0 ? 1 : 4)
        });
    }

    std::unordered_map<uint32_t, size_t> bytes_by_stream;
    size_t total = 0;
    for (size_t round = 0; round < 16 && total < kStreams * kPayloadBytes; ++round) {
        auto selected = Http2OutboundScheduler::pick_sendable_frames(H2OutboundBudget{
            .conn_window = static_cast<int32_t>(kStreams * kFrameBytes),
            .max_frame_size = static_cast<uint32_t>(kFrameBytes)
        }, streams, H2SchedulerConfig{
            .base_quantum = kFrameBytes
        });
        assert(!selected.frames.empty());
        total += selected.total_data_bytes;
        for (const auto& frame : selected.frames) {
            const auto* data = frame->as_data();
            assert(data);
            bytes_by_stream[data->stream_id()] += data->data().size();
        }
    }

    assert(total == kStreams * kPayloadBytes);
    assert(streams.front().stream_id == 1);
    assert(streams.back().stream_id == static_cast<uint32_t>(1 + (kStreams - 1) * 2));
    for (const auto& stream : streams) {
        assert(bytes_by_stream[stream.stream_id] == kPayloadBytes);
        assert(stream.pending.chunks.empty());
    }
}

void stress_large_body_without_data_loss()
{
    constexpr size_t kPayloadBytes = 1024 * 1024;
    constexpr size_t kFrameBytes = 4096;
    const std::string payload(kPayloadBytes, 'x');

    std::vector<H2StreamSendState> streams;
    streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {payload},
            .front_offset = 0,
            .end_stream = true
        },
        .stream_id = 1,
        .stream_window = static_cast<int32_t>(kPayloadBytes),
        .weight = 16
    });

    size_t total = 0;
    size_t frames = 0;
    while (total < kPayloadBytes) {
        auto selected = Http2OutboundScheduler::pick_sendable_frames(H2OutboundBudget{
            .conn_window = static_cast<int32_t>(kFrameBytes),
            .max_frame_size = static_cast<uint32_t>(kFrameBytes)
        }, streams, H2SchedulerConfig{
            .base_quantum = kFrameBytes
        });
        assert(selected.frames.size() == 1);
        const auto* data = selected.frames.front()->as_data();
        assert(data);
        assert(data->data().size() == kFrameBytes);
        total += selected.total_data_bytes;
        ++frames;

        if (total < kPayloadBytes) {
            assert(streams[0].pending.chunks.size() == 1);
            assert(streams[0].pending.chunks.front().size() == kPayloadBytes);
            assert(streams[0].pending.front_offset == total);
            assert(!data->is_end_stream());
        }
    }

    assert(frames == kPayloadBytes / kFrameBytes);
    assert(streams[0].pending.chunks.empty());
    assert(streams[0].pending.front_offset == 0);
}

void stress_window_updates()
{
    H2FlowController flow;
    constexpr size_t kStreams = 256;
    for (size_t i = 0; i < kStreams; ++i) {
        assert(flow.ensure_stream(static_cast<uint32_t>(1 + i * 2)).has_value());
    }

    for (size_t round = 0; round < 128; ++round) {
        const uint32_t stream_id = static_cast<uint32_t>(1 + (round % kStreams) * 2);
        const size_t sendable = flow.available_to_send(stream_id, 512, 512);
        assert(sendable > 0);
        assert(flow.consume_send_window(stream_id, sendable).has_value());
        assert(flow.apply_connection_window_update(static_cast<uint32_t>(sendable)).has_value());
        assert(flow.apply_stream_window_update(stream_id, static_cast<uint32_t>(sendable)).has_value());
    }

    auto overflow = flow.apply_connection_window_update(2147483647u - kDefaultInitialWindowSize + 1u);
    assert(!overflow.has_value());
    assert(overflow.error() == H2FlowControlError::WindowOverflow);
}

void stress_goaway_existing_streams()
{
    H2DispatcherConnectionState state;
    for (uint32_t stream_id = 1; stream_id <= 199; stream_id += 2) {
        Http2HeadersFrame headers;
        headers.header().stream_id = stream_id;
        headers.set_end_headers(true);
        auto result = Http2FrameDispatcher::dispatch(headers, state);
        assert(result.ok);
    }

    Http2GoAwayFrame goaway;
    goaway.set_last_stream_id(99);
    auto goaway_result = Http2FrameDispatcher::dispatch(goaway, state);
    assert(goaway_result.ok);
    assert(state.goaway_received);
    assert(state.goaway_last_stream_id == 99);

    for (uint32_t stream_id = 101; stream_id <= 199; stream_id += 2) {
        Http2DataFrame data;
        data.header().stream_id = stream_id;
        data.set_data("x");
        auto result = Http2FrameDispatcher::dispatch(data, state);
        assert(result.ok);
    }

    Http2HeadersFrame rejected;
    rejected.header().stream_id = 201;
    rejected.set_end_headers(true);
    auto rejected_result = Http2FrameDispatcher::dispatch(rejected, state);
    assert(!rejected_result.ok);
    assert(rejected_result.error_scope == H2DispatchErrorScope::Stream);
}

} // namespace

int main()
{
    stress_many_streams_fairness();
    stress_large_body_without_data_loss();
    stress_window_updates();
    stress_goaway_existing_streams();

    std::cout << "T85-H2KernelPressure PASS\n";
    return 0;
}
