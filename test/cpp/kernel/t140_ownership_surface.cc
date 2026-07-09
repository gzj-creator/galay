/**
 * @file t140_ownership_surface.cc
 * @brief 锁定 kernel owning/nontrivial 类型的 move-only 与显式 clone 表面。
 */

#include <galay/cpp/galay-kernel/async/aio_file.h>
#include <galay/cpp/galay-kernel/common/buffer.h>
#include <galay/cpp/galay-kernel/common/host.hpp>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/common/timer_manager.hpp>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>

using galay::kernel::Buffer;
using galay::kernel::Host;
using galay::kernel::RuntimeConfig;
using galay::kernel::RuntimeHandle;
using galay::kernel::SleepAwaitable;
using galay::kernel::TaskRef;
using galay::kernel::TimingWheelTimerManager;
using galay::kernel::Waker;
using galay::kernel::WithTimeout;
using galay::kernel::RecvAwaitable;

namespace {

template <typename T>
concept HasClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

static_assert(!std::is_copy_constructible_v<Buffer>);
static_assert(!std::is_copy_assignable_v<Buffer>);
static_assert(std::is_move_constructible_v<Buffer>);
static_assert(std::is_move_assignable_v<Buffer>);
static_assert(HasClone<Buffer>);

static_assert(!std::is_copy_constructible_v<TimingWheelTimerManager>);
static_assert(!std::is_copy_assignable_v<TimingWheelTimerManager>);
static_assert(std::is_move_constructible_v<TimingWheelTimerManager>);
static_assert(std::is_move_assignable_v<TimingWheelTimerManager>);

static_assert(!std::is_copy_constructible_v<SleepAwaitable>);
static_assert(!std::is_copy_assignable_v<SleepAwaitable>);
static_assert(std::is_move_constructible_v<SleepAwaitable>);
static_assert(std::is_move_assignable_v<SleepAwaitable>);

static_assert(!std::is_copy_constructible_v<WithTimeout<RecvAwaitable>>);
static_assert(!std::is_copy_assignable_v<WithTimeout<RecvAwaitable>>);
static_assert(std::is_move_constructible_v<WithTimeout<RecvAwaitable>>);
static_assert(std::is_move_assignable_v<WithTimeout<RecvAwaitable>>);

static_assert(std::is_copy_constructible_v<TaskRef>);
static_assert(std::is_copy_assignable_v<TaskRef>);
static_assert(std::is_copy_constructible_v<Waker>);
static_assert(std::is_copy_assignable_v<Waker>);
static_assert(std::is_copy_constructible_v<galay::kernel::detail::ResumeToken>);
static_assert(std::is_copy_assignable_v<galay::kernel::detail::ResumeToken>);
static_assert(std::is_copy_constructible_v<RuntimeHandle>);
static_assert(std::is_copy_assignable_v<RuntimeHandle>);
static_assert(std::is_copy_constructible_v<RuntimeConfig>);
static_assert(std::is_copy_assignable_v<RuntimeConfig>);
static_assert(std::is_copy_constructible_v<Host>);
static_assert(std::is_copy_assignable_v<Host>);

#ifdef USE_EPOLL
static_assert(!std::is_copy_constructible_v<galay::async::AioCommitAwaitable>);
static_assert(!std::is_copy_assignable_v<galay::async::AioCommitAwaitable>);
static_assert(std::is_move_constructible_v<galay::async::AioCommitAwaitable>);
static_assert(std::is_move_assignable_v<galay::async::AioCommitAwaitable>);
#endif

#ifdef USE_IOURING
static_assert(!std::is_copy_constructible_v<galay::kernel::ReadyRecvChunk>);
static_assert(!std::is_copy_assignable_v<galay::kernel::ReadyRecvChunk>);
static_assert(std::is_move_constructible_v<galay::kernel::ReadyRecvChunk>);
static_assert(std::is_move_assignable_v<galay::kernel::ReadyRecvChunk>);
#endif

bool bufferCloneDeepCopies()
{
    std::string payload = "kernel-buffer";
    Buffer original(payload);
    Buffer cloned = original.clone();

    if (cloned.toString() != payload) {
        std::cerr << "[T140] clone should preserve buffer bytes\n";
        return false;
    }
    if (cloned.data() == original.data()) {
        std::cerr << "[T140] clone should allocate independent storage\n";
        return false;
    }

    original.data()[0] = 'K';
    if (original.toString() != "Kernel-buffer") {
        std::cerr << "[T140] original mutation should be visible on original\n";
        return false;
    }
    if (cloned.toString() != payload) {
        std::cerr << "[T140] clone should not observe original mutation\n";
        return false;
    }
    return true;
}

bool movedFromBufferStaysDestructible()
{
    Buffer original(std::string("move-state"));
    Buffer moved(std::move(original));

    if (moved.toString() != "move-state") {
        std::cerr << "[T140] moved buffer should receive original bytes\n";
        return false;
    }
    if (original.data() != nullptr || original.length() != 0 || original.capacity() != 0) {
        std::cerr << "[T140] moved-from buffer should be empty and destructible\n";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    bool ok = true;
    ok = bufferCloneDeepCopies() && ok;
    ok = movedFromBufferStaysDestructible() && ok;
    if (ok) {
        std::cout << "T140-OwnershipSurface PASS\n";
    }
    return ok ? 0 : 1;
}
