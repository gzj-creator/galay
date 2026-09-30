/** @file t196_frame_default_alignment.cc
 *  @brief Cold frame alignment, payload bounds, failure and remote release.
 */
#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>

using namespace galay::kernel;

namespace {
constexpr std::size_t kAlignment = alignof(std::max_align_t);

bool require(bool condition, const char* message) {
    if (!condition) { std::cerr << "[T196] " << message << '\n'; }
    return condition;
}

bool verifyColdBoundaries() {
    constexpr std::array<std::size_t, 10> sizes{
        0, 1, 127, 128, 129, 2047, 2048, 2049, 4096, 65536};
    constexpr std::array<std::size_t, 9> alignments{
        0, 1, 2, 4, 8, kAlignment, kAlignment * 2, 64, 256};
    for (const auto alignment : alignments) {
        for (const auto size : sizes) {
            auto* frame = static_cast<std::byte*>(
                detail::allocateFrameStorage(size, alignment));
            if (!require(frame != nullptr, "cold allocation failed")) { return false; }
            const auto effective = alignment <= kAlignment ? kAlignment : alignment;
            bool valid = reinterpret_cast<std::uintptr_t>(frame) % effective == 0;
            for (std::size_t i = 0; i < size; ++i) {
                frame[i] = std::byte(i % 251);
            }
            for (std::size_t i = 0; i < size; ++i) {
                valid = valid && frame[i] == std::byte(i % 251);
            }
            detail::releaseFrameStorage(frame, size, alignment);
            if (!require(valid, "alignment or writable payload bounds changed")) {
                return false;
            }
        }
    }
    return true;
}

bool verifyFailures() {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    for (const auto alignment : {std::size_t(0), kAlignment, std::size_t(64)}) {
        void* overflow = detail::allocateFrameStorage(max, alignment);
        const bool rejected = overflow == nullptr;
        detail::releaseFrameStorage(overflow, max, alignment);
        if (!require(rejected, "overflow request must fail")) { return false; }
        detail::setFrameAllocationFailureForTesting(true);
        void* failed = detail::allocateFrameStorage(4096, alignment);
        detail::setFrameAllocationFailureForTesting(false);
        const bool injected = failed == nullptr;
        detail::releaseFrameStorage(failed, 4096, alignment);
        if (!require(injected, "injected allocation failure was bypassed")) {
            return false;
        }
        void* recovered = detail::allocateFrameStorage(4096, alignment);
        const bool valid = recovered != nullptr;
        detail::releaseFrameStorage(recovered, 4096, alignment);
        if (!require(valid, "allocation did not recover after failure")) { return false; }
    }
    return true;
}

bool verifyRemoteRelease() {
    for (const auto alignment : {std::size_t(0), kAlignment, std::size_t(64)}) {
        void* frame = detail::allocateFrameStorage(4096, alignment);
        if (!require(frame != nullptr, "remote-release allocation failed")) {
            return false;
        }
        std::thread consumer([=] {
            // The allocation header, not the supplied size, owns the base pointer.
            detail::releaseFrameStorage(frame, 1, alignment);
        });
        consumer.join();
    }
    return true;
}

Task<void> largeTask() {
    volatile std::byte payload[4096]{};
    co_await std::suspend_always{};
    payload[4095] = std::byte{1};
    co_return;
}

bool verifyLargeTaskLifecycle() {
    for (std::size_t resumes = 0; resumes <= 2; ++resumes) {
        auto task = largeTask();
        if (!require(task.isValid(), "large Task allocation failed")) { return false; }
        auto* state = detail::TaskAccess::taskRef(task).state();
        for (std::size_t i = 0; i < resumes; ++i) {
            if (!require(state->m_handle != nullptr, "large Task lost its frame")) {
                return false;
            }
            state->m_handle.resume();
        }
    }
    return true;
}
} // namespace

int main() {
    detail::setFrameRecyclerEnabledForTesting(false);
    const bool cold = verifyColdBoundaries() && verifyFailures();
    detail::setFrameRecyclerEnabledForTesting(true);
    if (!cold || !verifyRemoteRelease() || !verifyLargeTaskLifecycle()) { return 1; }
    std::cout << "T196-FrameDefaultAlignment PASS\n";
    return 0;
}
