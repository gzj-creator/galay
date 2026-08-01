#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/async/async_mutex.h>
#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>
#include <concepts>
#include <coroutine>
#include <type_traits>

using galay::kernel::AsyncMutexAwaitable;
using galay::kernel::AsyncWaiterAwaitable;
using galay::kernel::TaskPromise;
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
static_assert(TaskPromiseSuspendible<galay::mpsc::UnboundedRecvAwaitable<int>>,
              "mpsc::UnboundedRecvAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<galay::mpsc::UnboundedRecvBatchAwaitable<int>>,
              "mpsc::UnboundedRecvBatchAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<galay::spsc::UnboundedRecvAwaitable<int>>,
              "spsc::UnboundedRecvAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<galay::spsc::UnboundedRecvBatchAwaitable<int>>,
              "spsc::UnboundedRecvBatchAwaitable should suspend with Task promise handles");
static_assert(TaskPromiseSuspendible<galay::spsc::UnboundedRecvBatchedAwaitable<int>>,
              "spsc::UnboundedRecvBatchedAwaitable should suspend with Task promise handles");

int main()
{
    return 0;
}
