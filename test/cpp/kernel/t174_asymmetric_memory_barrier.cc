/**
 * @file t174_asymmetric_memory_barrier.cc
 * @brief 验证 Linux 非对称内存屏障能力探测与重屏障返回值。
 */

#include <galay/cpp/galay-kernel/concurrency/detail/asymmetric_memory_barrier.h>

#include <iostream>

int main()
{
    const auto& support =
        galay::kernel::detail::asymmetric_memory_barrier_support();
#if defined(__linux__)
    if (support) {
        galay::kernel::detail::asymmetric_light_barrier();
        auto barrier = galay::kernel::detail::asymmetric_heavy_barrier();
        if (!barrier) {
            std::cerr << "T174 heavy barrier failed system_error="
                      << barrier.error().systemError << '\n';
            return 1;
        }
    }
#else
    if (support) {
        std::cerr << "T174 unexpectedly enabled Linux membarrier\n";
        return 1;
    }
#endif
    std::cout << "T174-AsymmetricMemoryBarrier PASS native="
              << support.has_value() << '\n';
    return 0;
}
