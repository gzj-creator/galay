/**
 * @file b14_h2_kernel_pressure.cc
 * @brief HTTP/2 kernel dispatcher/flow/scheduler pressure benchmark
 */

#include <galay/cpp/galay-http2/kernel/flow_control.h>
#include <galay/cpp/galay-http2/kernel/frame_disp.h>
#include <galay/cpp/galay-http2/kernel/h2_core.h>
#include <galay/cpp/galay-http2/kernel/out_sched.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace galay::http2;

namespace
{

struct BenchResult
{
    size_t scheduler_bytes = 0;
    size_t scheduler_frames = 0;
    size_t scheduler_streams = 0;
    size_t bytes_scheduler_bytes = 0;
    size_t bytes_scheduler_frames = 0;
    size_t bytes_scheduler_streams = 0;
    size_t core_frame_bytes = 0;
    size_t core_frame_frames = 0;
    size_t core_frame_streams = 0;
    size_t core_bytes_bytes = 0;
    size_t core_bytes_frames = 0;
    size_t core_bytes_streams = 0;
    size_t flow_ops = 0;
    size_t flow_rounds = 0;
    size_t dispatch_frames = 0;
    double elapsed_ms = 0.0;
    double scheduler_ms = 0.0;
    double bytes_scheduler_ms = 0.0;
    double core_frame_ms = 0.0;
    double core_bytes_ms = 0.0;
    double flow_ms = 0.0;
    double dispatch_ms = 0.0;
};

std::vector<H2StreamSendState> makeStreams(size_t streams_count, size_t payload_bytes)
{
    std::vector<H2StreamSendState> streams;
    streams.reserve(streams_count);
    for (size_t i = 0; i < streams_count; ++i) {
        streams.push_back(H2StreamSendState{
            .pending = {
                .chunks = {std::string(payload_bytes, static_cast<char>('a' + (i % 26)))},
                .front_offset = 0,
                .end_stream = false
            },
            .stream_id = static_cast<uint32_t>(1 + i * 2),
            .stream_window = static_cast<int32_t>(payload_bytes),
            .weight = static_cast<uint8_t>((i % 4) + 1)
        });
    }
    return streams;
}

bool runSchedulerPressure(size_t streams_count,
                          size_t payload_bytes,
                          size_t frame_bytes,
                          BenchResult& result)
{
    auto streams = makeStreams(streams_count, payload_bytes);
    std::unordered_map<uint32_t, size_t> bytes_by_stream;
    const size_t expected_bytes = streams_count * payload_bytes;
    while (result.scheduler_bytes < expected_bytes) {
        auto selected = Http2OutboundScheduler::pickSendableFrames(H2OutboundBudget{
            .conn_window = static_cast<int32_t>(streams_count * frame_bytes),
            .max_frame_size = static_cast<uint32_t>(frame_bytes)
        }, streams, H2SchedulerConfig{
            .base_quantum = frame_bytes
        });
        if (selected.frames.empty()) {
            std::cerr << "scheduler made no progress\n";
            return false;
        }

        result.scheduler_bytes += selected.total_data_bytes;
        result.scheduler_frames += selected.frames.size();
        for (const auto& frame : selected.frames) {
            const auto* data = frame->asData();
            if (!data) {
                std::cerr << "scheduler emitted non-DATA frame in DATA benchmark\n";
                return false;
            }
            bytes_by_stream[data->streamId()] += data->data().size();
        }
    }

    for (const auto& stream : streams) {
        if (bytes_by_stream[stream.stream_id] != payload_bytes) {
            std::cerr << "stream " << stream.stream_id << " bytes mismatch\n";
            return false;
        }
    }
    result.scheduler_streams = streams_count;
    return true;
}

bool runCoreFramePressure(size_t streams_count,
                          size_t payload_bytes,
                          size_t frame_bytes,
                          BenchResult& result)
{
    Http2ConnectionCore core;
    for (size_t i = 0; i < streams_count; ++i) {
        core.enqueueData(static_cast<uint32_t>(1 + i * 2),
                         std::string(payload_bytes, static_cast<char>('a' + (i % 26))),
                         false,
                         static_cast<uint8_t>((i % 4) + 1));
    }

    const size_t expected_bytes = streams_count * payload_bytes;
    while (result.core_frame_bytes < expected_bytes) {
        auto selected = core.flushOutbound(H2OutboundBudget{
            .conn_window = static_cast<int32_t>(streams_count * frame_bytes),
            .max_frame_size = static_cast<uint32_t>(frame_bytes)
        }, H2SchedulerConfig{
            .base_quantum = frame_bytes
        });
        if (selected.frames.empty()) {
            std::cerr << "core frame flush made no progress\n";
            return false;
        }

        result.core_frame_bytes += selected.total_data_bytes;
        result.core_frame_frames += selected.frames.size();
        for (const auto& frame : selected.frames) {
            if (!frame->isData()) {
                std::cerr << "core frame flush emitted non-DATA frame in DATA benchmark\n";
                return false;
            }
        }
    }
    if (core.hasOutboundWork()) {
        std::cerr << "core frame flush left outbound work after expected bytes\n";
        return false;
    }
    result.core_frame_streams = streams_count;
    return true;
}

bool runBytesSchedulerPressure(size_t streams_count,
                               size_t payload_bytes,
                               size_t frame_bytes,
                               BenchResult& result)
{
    auto streams = makeStreams(streams_count, payload_bytes);
    const size_t expected_bytes = streams_count * payload_bytes;
    while (result.bytes_scheduler_bytes < expected_bytes) {
        auto selected = Http2OutboundScheduler::pickSendableBytes(H2OutboundBudget{
            .conn_window = static_cast<int32_t>(streams_count * frame_bytes),
            .max_frame_size = static_cast<uint32_t>(frame_bytes)
        }, streams, H2SchedulerConfig{
            .base_quantum = frame_bytes
        });
        if (selected.frames.empty()) {
            std::cerr << "bytes scheduler made no progress\n";
            return false;
        }

        result.bytes_scheduler_bytes += selected.total_data_bytes;
        result.bytes_scheduler_frames += selected.frames.size();
        for (const auto& frame_bytes_out : selected.frames) {
            if (frame_bytes_out.size() < kHttp2FrameHeaderLength) {
                std::cerr << "bytes scheduler emitted truncated frame\n";
                return false;
            }
        }
    }
    result.bytes_scheduler_streams = streams_count;
    return true;
}

bool runCoreBytesPressure(size_t streams_count,
                          size_t payload_bytes,
                          size_t frame_bytes,
                          BenchResult& result)
{
    Http2ConnectionCore core;
    for (size_t i = 0; i < streams_count; ++i) {
        core.enqueueData(static_cast<uint32_t>(1 + i * 2),
                         std::string(payload_bytes, static_cast<char>('a' + (i % 26))),
                         false,
                         static_cast<uint8_t>((i % 4) + 1));
    }

    const size_t expected_bytes = streams_count * payload_bytes;
    while (result.core_bytes_bytes < expected_bytes) {
        auto selected = core.flushOutboundBytes(H2OutboundBudget{
            .conn_window = static_cast<int32_t>(streams_count * frame_bytes),
            .max_frame_size = static_cast<uint32_t>(frame_bytes)
        }, H2SchedulerConfig{
            .base_quantum = frame_bytes
        });
        if (selected.frames.empty()) {
            std::cerr << "core bytes flush made no progress\n";
            return false;
        }

        result.core_bytes_bytes += selected.total_data_bytes;
        result.core_bytes_frames += selected.frames.size();
        for (const auto& frame_bytes_out : selected.frames) {
            if (frame_bytes_out.size() < kHttp2FrameHeaderLength) {
                std::cerr << "core bytes flush emitted truncated frame\n";
                return false;
            }
        }
    }
    if (core.hasOutboundWork()) {
        std::cerr << "core bytes flush left outbound work after expected bytes\n";
        return false;
    }
    result.core_bytes_streams = streams_count;
    return true;
}

bool runFlowPressure(size_t streams_count, size_t rounds, BenchResult& result)
{
    H2FlowController flow;
    for (size_t i = 0; i < streams_count; ++i) {
        auto ok = flow.ensureStream(static_cast<uint32_t>(1 + i * 2));
        if (!ok) {
            return false;
        }
    }

    for (size_t i = 0; i < rounds; ++i) {
        const auto stream_id = static_cast<uint32_t>(1 + (i % streams_count) * 2);
        const size_t bytes = flow.availableToSend(stream_id, 1024, 1024);
        if (bytes == 0) {
            std::cerr << "flow returned zero sendable bytes\n";
            return false;
        }
        if (!flow.consumeSendWindow(stream_id, bytes)) {
            return false;
        }
        if (!flow.applyConnectionWindowUpdate(static_cast<uint32_t>(bytes))) {
            return false;
        }
        if (!flow.applyStreamWindowUpdate(stream_id, static_cast<uint32_t>(bytes))) {
            return false;
        }
        result.flow_ops += 4;
    }
    result.flow_rounds = rounds;
    return true;
}

bool runDispatchPressure(size_t streams_count, BenchResult& result)
{
    H2DispatcherConnectionState state;
    for (uint32_t stream_id = 1; stream_id <= streams_count * 2; stream_id += 2) {
        Http2HeadersFrame headers;
        headers.header().stream_id = stream_id;
        headers.setEndHeaders(true);
        if (!Http2FrameDispatcher::dispatch(headers, state).ok) {
            return false;
        }
        ++result.dispatch_frames;
    }

    Http2GoAwayFrame goaway;
    goaway.setLastStreamId(static_cast<uint32_t>(streams_count));
    if (!Http2FrameDispatcher::dispatch(goaway, state).ok) {
        return false;
    }
    ++result.dispatch_frames;

    for (uint32_t stream_id = 1; stream_id <= streams_count * 2; stream_id += 2) {
        Http2DataFrame data;
        data.header().stream_id = stream_id;
        data.setData("x");
        auto r = Http2FrameDispatcher::dispatch(data, state);
        if (!r.ok && state.streams.find(stream_id) != state.streams.end()) {
            return false;
        }
        ++result.dispatch_frames;
    }
    return true;
}

BenchResult runBench(size_t streams_count, size_t payload_bytes, size_t flow_rounds)
{
    BenchResult result;
    const auto start = std::chrono::steady_clock::now();
    const auto scheduler_start = std::chrono::steady_clock::now();
    const bool scheduler_ok = runSchedulerPressure(streams_count, payload_bytes, 16, result);
    const auto scheduler_end = std::chrono::steady_clock::now();

    const auto bytes_scheduler_start = std::chrono::steady_clock::now();
    const bool bytes_scheduler_ok = scheduler_ok &&
        runBytesSchedulerPressure(streams_count, payload_bytes, 16, result);
    const auto bytes_scheduler_end = std::chrono::steady_clock::now();

    const auto core_frame_start = std::chrono::steady_clock::now();
    const bool core_frame_ok = bytes_scheduler_ok &&
        runCoreFramePressure(streams_count, payload_bytes, 16, result);
    const auto core_frame_end = std::chrono::steady_clock::now();

    const auto core_bytes_start = std::chrono::steady_clock::now();
    const bool core_bytes_ok = core_frame_ok &&
        runCoreBytesPressure(streams_count, payload_bytes, 16, result);
    const auto core_bytes_end = std::chrono::steady_clock::now();

    const auto flow_start = std::chrono::steady_clock::now();
    const bool flow_ok = core_bytes_ok && runFlowPressure(streams_count, flow_rounds, result);
    const auto flow_end = std::chrono::steady_clock::now();

    const auto dispatch_start = std::chrono::steady_clock::now();
    const bool dispatch_ok = flow_ok && runDispatchPressure(streams_count, result);
    const auto dispatch_end = std::chrono::steady_clock::now();

    const auto end = std::chrono::steady_clock::now();
    result.scheduler_ms = std::chrono::duration<double, std::milli>(scheduler_end - scheduler_start).count();
    result.bytes_scheduler_ms = std::chrono::duration<double, std::milli>(bytes_scheduler_end - bytes_scheduler_start).count();
    result.core_frame_ms = std::chrono::duration<double, std::milli>(core_frame_end - core_frame_start).count();
    result.core_bytes_ms = std::chrono::duration<double, std::milli>(core_bytes_end - core_bytes_start).count();
    result.flow_ms = std::chrono::duration<double, std::milli>(flow_end - flow_start).count();
    result.dispatch_ms = std::chrono::duration<double, std::milli>(dispatch_end - dispatch_start).count();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (!dispatch_ok) {
        result.elapsed_ms = -1.0;
    }
    return result;
}

double perSecond(size_t value, double elapsed_ms)
{
    if (elapsed_ms <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(value) / (elapsed_ms / 1000.0);
}

double mibPerSecond(size_t bytes, double elapsed_ms)
{
    return perSecond(bytes, elapsed_ms) / (1024.0 * 1024.0);
}

const char* workloadHotStage(const BenchResult& result)
{
    if (result.scheduler_ms >= result.flow_ms && result.scheduler_ms >= result.dispatch_ms) {
        return "scheduler";
    }
    if (result.flow_ms >= result.dispatch_ms) {
        return "flow_control";
    }
    return "dispatcher";
}

const char* throughputBottleneckStage(const BenchResult& result)
{
    const double scheduler_qps = perSecond(result.scheduler_frames, result.scheduler_ms);
    const double bytes_scheduler_qps = perSecond(result.bytes_scheduler_frames, result.bytes_scheduler_ms);
    const double flow_qps = perSecond(result.flow_rounds, result.flow_ms);
    const double dispatch_qps = perSecond(result.dispatch_frames, result.dispatch_ms);
    if (std::min(scheduler_qps, bytes_scheduler_qps) <= flow_qps &&
        std::min(scheduler_qps, bytes_scheduler_qps) <= dispatch_qps) {
        return "scheduler";
    }
    if (flow_qps <= dispatch_qps) {
        return "flow_control";
    }
    return "dispatcher";
}

} // namespace

int main(int argc, char* argv[])
{
    size_t streams = 1000;
    size_t payload_bytes = 128;
    size_t flow_rounds = 100000;
    if (argc > 1) {
        streams = static_cast<size_t>(std::stoul(argv[1]));
    }
    if (argc > 2) {
        payload_bytes = static_cast<size_t>(std::stoul(argv[2]));
    }
    if (argc > 3) {
        flow_rounds = static_cast<size_t>(std::stoul(argv[3]));
    }

    auto result = runBench(streams, payload_bytes, flow_rounds);
    if (result.elapsed_ms < 0.0) {
        return 1;
    }

    std::cout << "HTTP/2 kernel pressure benchmark\n";
    std::cout << "streams=" << streams
              << " payload_bytes=" << payload_bytes
              << " flow_rounds=" << flow_rounds << "\n";
    std::cout << "elapsed_ms=" << result.elapsed_ms << "\n";
    std::cout << "workload_hot_stage=" << workloadHotStage(result) << "\n";
    std::cout << "throughput_bottleneck_stage=" << throughputBottleneckStage(result) << "\n";
    std::cout << "scheduler_ms=" << result.scheduler_ms
              << " scheduler_stream_qps=" << perSecond(result.scheduler_streams, result.scheduler_ms)
              << " scheduler_frame_qps=" << perSecond(result.scheduler_frames, result.scheduler_ms)
              << " scheduler_mib_per_s=" << mibPerSecond(result.scheduler_bytes, result.scheduler_ms)
              << "\n";
    std::cout << "bytes_scheduler_ms=" << result.bytes_scheduler_ms
              << " bytes_scheduler_stream_qps="
              << perSecond(result.bytes_scheduler_streams, result.bytes_scheduler_ms)
              << " bytes_scheduler_frame_qps="
              << perSecond(result.bytes_scheduler_frames, result.bytes_scheduler_ms)
              << " bytes_scheduler_mib_per_s="
              << mibPerSecond(result.bytes_scheduler_bytes, result.bytes_scheduler_ms)
              << "\n";
    std::cout << "core_frame_ms=" << result.core_frame_ms
              << " core_frame_stream_qps=" << perSecond(result.core_frame_streams, result.core_frame_ms)
              << " core_frame_frame_qps=" << perSecond(result.core_frame_frames, result.core_frame_ms)
              << " core_frame_mib_per_s=" << mibPerSecond(result.core_frame_bytes, result.core_frame_ms)
              << "\n";
    std::cout << "core_bytes_ms=" << result.core_bytes_ms
              << " core_bytes_stream_qps=" << perSecond(result.core_bytes_streams, result.core_bytes_ms)
              << " core_bytes_frame_qps=" << perSecond(result.core_bytes_frames, result.core_bytes_ms)
              << " core_bytes_mib_per_s=" << mibPerSecond(result.core_bytes_bytes, result.core_bytes_ms)
              << "\n";
    std::cout << "flow_ms=" << result.flow_ms
              << " flow_round_qps=" << perSecond(result.flow_rounds, result.flow_ms)
              << " flow_ops_per_s=" << perSecond(result.flow_ops, result.flow_ms)
              << "\n";
    std::cout << "dispatch_ms=" << result.dispatch_ms
              << " dispatch_frame_qps=" << perSecond(result.dispatch_frames, result.dispatch_ms)
              << "\n";
    std::cout << "scheduler_bytes=" << result.scheduler_bytes
              << " scheduler_frames=" << result.scheduler_frames
              << "\n";
    std::cout << "bytes_scheduler_bytes=" << result.bytes_scheduler_bytes
              << " bytes_scheduler_frames=" << result.bytes_scheduler_frames
              << "\n";
    std::cout << "core_frame_bytes=" << result.core_frame_bytes
              << " core_frame_frames=" << result.core_frame_frames
              << "\n";
    std::cout << "core_bytes_bytes=" << result.core_bytes_bytes
              << " core_bytes_frames=" << result.core_bytes_frames
              << "\n";
    std::cout << "flow_ops=" << result.flow_ops
              << " flow_rounds=" << result.flow_rounds << "\n";
    std::cout << "dispatch_frames=" << result.dispatch_frames << "\n";
    return 0;
}
