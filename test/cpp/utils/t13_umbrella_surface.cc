/**
 * @file t13_umbrella_surface.cc
 * @brief 验证 galay-utils 总头暴露已支持的公共工具类型。
 */

#include <galay/cpp/galay-utils/galay_utils.hpp>

#include <iostream>
#include <utility>

int main()
{
    galay::utils::CountingSemaphore semaphore(2);
    if (!semaphore.tryAcquire(2)) {
        std::cerr << "[t13] CountingSemaphore should be visible through galay_utils.hpp\n";
        return 1;
    }
    if (semaphore.tryAcquire(1)) {
        std::cerr << "[t13] CountingSemaphore boundary should still enforce capacity\n";
        return 1;
    }

    galay::utils::TokenBucketLimiter limiter(0.0, 1);
    if (!limiter.tryAcquire(1) || limiter.tryAcquire(1)) {
        std::cerr << "[t13] TokenBucketLimiter should be visible and enforce capacity\n";
        return 1;
    }

    galay::utils::SpscRingBuffer<int> ring(2);
    int value = 7;
    if (ring.error() != galay::utils::SpscRingBufferError::kNone ||
        !ring.trySend(std::move(value))) {
        std::cerr << "[t13] SpscRingBuffer should be visible through galay_utils.hpp\n";
        return 1;
    }
    auto received = ring.tryRecv();
    if (!received.has_value() || *received != 7) {
        std::cerr << "[t13] SpscRingBuffer umbrella surface should preserve values\n";
        return 1;
    }

    return 0;
}
