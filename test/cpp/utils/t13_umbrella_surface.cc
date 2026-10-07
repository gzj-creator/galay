/**
 * @file t13_umbrella_surface.cc
 * @brief 验证 galay-utils 总头暴露已支持的公共工具类型。
 */

#include <galay/cpp/galay-utils/galay_utils.hpp>

#include <iostream>
#include <utility>

int main()
{
    const auto invalidEnv = galay::utils::Env::get("");
    if (invalidEnv || invalidEnv.error() != std::errc::invalid_argument) {
        std::cerr << "[t13] Env should be visible and reject invalid names\n";
        return 1;
    }
    const auto pageSize = galay::utils::Memory::page_size();
    if (!pageSize || *pageSize == 0) {
        return 1;
    }
    static_assert(galay::utils::Numa::kMaxNodes > 0);
    static_assert(galay::utils::Memory::kMaxNodes > 0);
    galay::utils::CountingSemaphore semaphore(2);
    if (!semaphore.try_acquire(2)) {
        std::cerr << "[t13] CountingSemaphore should be visible through galay_utils.hpp\n";
        return 1;
    }
    if (semaphore.try_acquire(1)) {
        std::cerr << "[t13] CountingSemaphore boundary should still enforce capacity\n";
        return 1;
    }

    galay::utils::TokenBucketLimiter limiter(0.0, 1);
    if (!limiter.try_acquire(1) || limiter.try_acquire(1)) {
        std::cerr << "[t13] TokenBucketLimiter should be visible and enforce capacity\n";
        return 1;
    }

    galay::utils::TypeRingBuffer<int> ring(2);
    int value = 7;
    if (ring.error() != galay::utils::TypeRingBufferError::kNone ||
        !ring.try_write(std::move(value))) {
        std::cerr << "[t13] TypeRingBuffer should be visible through galay_utils.hpp\n";
        return 1;
    }
    auto received = ring.try_read();
    if (!received.has_value() || *received != 7) {
        std::cerr << "[t13] TypeRingBuffer umbrella surface should preserve values\n";
        return 1;
    }

    return 0;
}
