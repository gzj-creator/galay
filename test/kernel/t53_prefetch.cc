/**
 * @file t53_prefetch.cc
 * @brief 用途：验证 `MpscChannel` 单条接收路径的预取优化不会破坏语义。
 * 关键覆盖点：单次接收预取、backlog 消耗、预取上限与最终一致性。
 * 通过条件：预取优化下消息仍完整可见，测试返回 0。
 */

#include "galay-kernel/concurrency/mpsc_channel.h"
#include "test/mpsc_access.h"

#include <iostream>
#include <vector>

using namespace galay::kernel;

int main() {
    constexpr int kMessageCount = 10;
    constexpr size_t kDefaultBatchSize = 4;
    constexpr size_t kPrefetchLimit = 3;

    MpscChannel<int> channel(kDefaultBatchSize, kPrefetchLimit);
    for (int i = 0; i < kMessageCount; ++i) {
        if (!channel.send(i)) {
            std::cerr << "[T53] failed to preload message " << i << "\n";
            return 1;
        }
    }

    auto first = channel.tryRecv();
    if (!first || *first != 0) {
        std::cerr << "[T53] expected first value 0\n";
        return 1;
    }

    const size_t prefetched = MpscChannelTestAccess::prefetchedCount(channel);
    if (prefetched == 0) {
        std::cerr << "[T53] expected single recv prefetch buffer to be populated\n";
        return 1;
    }
    if (prefetched > kPrefetchLimit) {
        std::cerr << "[T53] prefetch buffer exceeded configured limit\n";
        return 1;
    }

    if (channel.size() != static_cast<size_t>(kMessageCount - 1)) {
        std::cerr << "[T53] expected size to stay at " << (kMessageCount - 1)
                  << ", got " << channel.size() << "\n";
        return 1;
    }

    auto firstBatch = channel.tryRecvBatch();
    if (!firstBatch || firstBatch->size() != kDefaultBatchSize) {
        std::cerr << "[T53] expected no-arg tryRecvBatch to use configured batch size\n";
        return 1;
    }

    for (size_t i = 0; i < kDefaultBatchSize; ++i) {
        if ((*firstBatch)[i] != static_cast<int>(i + 1)) {
            std::cerr << "[T53] ordering mismatch at index " << (i - 1) << "\n";
            return 1;
        }
    }

    auto rest = channel.tryRecvBatch(kMessageCount);
    if (!rest || rest->size() != static_cast<size_t>(kMessageCount - 1 - kDefaultBatchSize)) {
        std::cerr << "[T53] expected second batch to drain remaining messages\n";
        return 1;
    }

    for (size_t i = 0; i < rest->size(); ++i) {
        const int expected = static_cast<int>(kDefaultBatchSize + 1 + i);
        if ((*rest)[i] != expected) {
            std::cerr << "[T53] trailing ordering mismatch at index " << i << "\n";
            return 1;
        }
    }

    if (MpscChannelTestAccess::prefetchedCount(channel) != 0) {
        std::cerr << "[T53] expected prefetch buffer to be empty after full drain\n";
        return 1;
    }

    if (!channel.empty()) {
        std::cerr << "[T53] expected channel to be empty after draining all messages\n";
        return 1;
    }

    std::cout << "T53-MpscSingleRecvPrefetch PASS\n";
    return 0;
}
