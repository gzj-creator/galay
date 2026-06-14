#include "kernel/common/sleep.hpp"
#include "kernel/concurrency/async_mutex.h"
#include "kernel/concurrency/async_waiter.h"
#include "kernel/concurrency/mpsc_channel.h"
#include "kernel/concurrency/unsafe_channel.h"
#include "kernel/kernel/task.h"
#include "kernel/kernel/waker.h"
#include <concepts>
#include <coroutine>
#include <type_traits>

using galay::kernel::AsyncMutexAwaitable;
using galay::kernel::AsyncWaiterAwaitable;
using galay::kernel::MpscRecvAwaitable;
using galay::kernel::MpscRecvBatchAwaitable;
using galay::kernel::TaskPromise;
using galay::kernel::UnsafeRecvAwaitable;
using galay::kernel::UnsafeRecvBatchAwaitable;
using galay::kernel::UnsafeRecvBatchedAwaitable;
using galay::kernel::Waker;
using galay::kernel::SleepAwaitable;

template <typename Awaitable>
concept TaskPromiseSuspendible = requires(Awaitable& awaitable,
                                          std::coroutine_handle<TaskPromise<void>> handle) {
    { awaitable.await_suspend(handle) } -> std::same_as<bool>;
};

static_assert(std::constructible_from<Waker, std::coroutine_handle<TaskPromise<void>>>,
              "Waker should accept typed Task promise handles");
static_assert(TaskPromiseSuspendible<SleepAwaitable>,
              "SleepAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<AsyncMutexAwaitable>,
              "AsyncMutexAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<AsyncWaiterAwaitable<int>>,
              "AsyncWaiterAwaitable<T> should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<AsyncWaiterAwaitable<void>>,
              "AsyncWaiterAwaitable<void> should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<MpscRecvAwaitable<int>>,
              "MpscRecvAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<MpscRecvBatchAwaitable<int>>,
              "MpscRecvBatchAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<UnsafeRecvAwaitable<int>>,
              "UnsafeRecvAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<UnsafeRecvBatchAwaitable<int>>,
              "UnsafeRecvBatchAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<UnsafeRecvBatchedAwaitable<int>>,
              "UnsafeRecvBatchedAwaitable should suspend with Task promise handles");

int main()
{
    return 0;
}
