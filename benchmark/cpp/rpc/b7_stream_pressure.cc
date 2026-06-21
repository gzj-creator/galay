#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::rpc;

int main(int argc, char** argv)
{
    size_t frames = 10000;
    size_t payload_size = 128;
    if (argc > 1) {
        frames = static_cast<size_t>(std::stoull(argv[1]));
    }
    if (argc > 2) {
        payload_size = static_cast<size_t>(std::stoull(argv[2]));
    }

    RpcStreamLimits limits;
    limits.max_frame_bytes = std::max<size_t>(payload_size, 1);
    std::string payload(payload_size, 'x');
    std::vector<std::chrono::nanoseconds> latencies;
    latencies.reserve(frames);

    size_t errors = 0;
    size_t bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < frames; ++i) {
        const auto frame_start = std::chrono::steady_clock::now();
        StreamMessage frame(static_cast<uint32_t>(i + 1), payload.data(), payload.size());
        auto valid = frame.validate(limits);
        if (!valid.has_value()) {
            ++errors;
            continue;
        }
        auto wire = frame.serialize(RpcMessageType::STREAM_DATA);
        bytes += wire.size();
        latencies.push_back(std::chrono::steady_clock::now() - frame_start);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&](double p) -> long long {
        if (latencies.empty()) {
            return 0;
        }
        const auto index = std::min(latencies.size() - 1,
                                    static_cast<size_t>((latencies.size() - 1) * p));
        return std::chrono::duration_cast<std::chrono::nanoseconds>(latencies[index]).count();
    };

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double frames_per_sec = seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0;
    const double bytes_per_sec = seconds > 0.0 ? static_cast<double>(bytes) / seconds : 0.0;

    std::cout << "RPC stream pressure benchmark\n"
              << "frames=" << frames << "\n"
              << "payload=" << payload_size << "\n"
              << "frames_per_sec=" << frames_per_sec << "\n"
              << "bytes_per_sec=" << bytes_per_sec << "\n"
              << "p50_ns=" << percentile(0.50) << "\n"
              << "p90_ns=" << percentile(0.90) << "\n"
              << "p99_ns=" << percentile(0.99) << "\n"
              << "errors=" << errors << "\n";
    return errors == 0 ? 0 : 1;
}
