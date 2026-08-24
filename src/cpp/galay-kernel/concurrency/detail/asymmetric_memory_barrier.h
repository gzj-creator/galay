/**
 * @file asymmetric_memory_barrier.h
 * @brief Linux process-wide heavy barrier with an explicit unsupported fallback.
 */

#ifndef GALAY_KERNEL_CONCURRENCY_DETAIL_ASYMMETRIC_MEMORY_BARRIER_H
#define GALAY_KERNEL_CONCURRENCY_DETAIL_ASYMMETRIC_MEMORY_BARRIER_H

#include "../../common/kernel_config.h"

#include <atomic>
#include <cstdint>
#include <expected>

#if GALAY_KERNEL_HAS_LINUX_MEMBARRIER
#include <cerrno>
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace galay::kernel::detail
{

enum class AsymmetricMemoryBarrierErrorCode : uint8_t {
    kUnsupported,
    kQueryFailed,
    kRegistrationFailed,
    kBarrierFailed,
};

struct AsymmetricMemoryBarrierError {
    AsymmetricMemoryBarrierErrorCode code;
    int systemError;
};

using AsymmetricMemoryBarrierResult =
    std::expected<void, AsymmetricMemoryBarrierError>;

/**
 * @brief Probe and register Linux PRIVATE_EXPEDITED membarrier once per process.
 * @return success when later heavy barriers are supported; otherwise a typed
 *         capability or syscall error. Non-Linux targets return kUnsupported.
 */
[[nodiscard]] inline const AsymmetricMemoryBarrierResult&
asymmetricMemoryBarrierSupport() noexcept
{
    static const AsymmetricMemoryBarrierResult support = []() noexcept
        -> AsymmetricMemoryBarrierResult {
#if GALAY_KERNEL_HAS_LINUX_MEMBARRIER && defined(SYS_membarrier)
        const long query = ::syscall(SYS_membarrier,
                                     MEMBARRIER_CMD_QUERY,
                                     0,
                                     0);
        if (query < 0) {
            return std::unexpected(AsymmetricMemoryBarrierError{
                AsymmetricMemoryBarrierErrorCode::kQueryFailed, errno});
        }
        constexpr long kRequired = MEMBARRIER_CMD_PRIVATE_EXPEDITED |
            MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
        if ((query & kRequired) != kRequired) {
            return std::unexpected(AsymmetricMemoryBarrierError{
                AsymmetricMemoryBarrierErrorCode::kUnsupported, 0});
        }
        if (::syscall(SYS_membarrier,
                      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
                      0,
                      0) != 0) {
            return std::unexpected(AsymmetricMemoryBarrierError{
                AsymmetricMemoryBarrierErrorCode::kRegistrationFailed,
                errno});
        }
        if (::syscall(SYS_membarrier,
                      MEMBARRIER_CMD_PRIVATE_EXPEDITED,
                      0,
                      0) != 0) {
            return std::unexpected(AsymmetricMemoryBarrierError{
                AsymmetricMemoryBarrierErrorCode::kBarrierFailed, errno});
        }
        return {};
#else
        return std::unexpected(AsymmetricMemoryBarrierError{
            AsymmetricMemoryBarrierErrorCode::kUnsupported, 0});
#endif
    }();
    return support;
}

/**
 * @brief Compiler-side half of the Linux asymmetric barrier protocol.
 * @pre asymmetricMemoryBarrierSupport() returned success.
 */
inline void asymmetricLightBarrier() noexcept
{
#if GALAY_KERNEL_HAS_LINUX_MEMBARRIER && \
    (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @brief Force every running thread in this process through a full barrier.
 * @return success, or the capability/syscall error without hiding errno.
 */
[[nodiscard]] inline AsymmetricMemoryBarrierResult
asymmetricHeavyBarrier() noexcept
{
    const auto& support = asymmetricMemoryBarrierSupport();
    if (!support) {
        return std::unexpected(support.error());
    }
#if GALAY_KERNEL_HAS_LINUX_MEMBARRIER && defined(SYS_membarrier)
    if (::syscall(SYS_membarrier,
                  MEMBARRIER_CMD_PRIVATE_EXPEDITED,
                  0,
                  0) != 0) {
        return std::unexpected(AsymmetricMemoryBarrierError{
            AsymmetricMemoryBarrierErrorCode::kBarrierFailed, errno});
    }
    return {};
#else
    return std::unexpected(AsymmetricMemoryBarrierError{
        AsymmetricMemoryBarrierErrorCode::kUnsupported, 0});
#endif
}

} // namespace galay::kernel::detail

#endif // GALAY_KERNEL_CONCURRENCY_DETAIL_ASYMMETRIC_MEMORY_BARRIER_H
