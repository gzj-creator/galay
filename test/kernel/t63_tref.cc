/**
 * @file t63_tref.cc
 * @brief 用途：锁定 `Scheduler` 公开提交面只暴露 `TaskRef` 风格接口。
 * 关键覆盖点：`schedule(TaskRef)`、`scheduleDeferred(TaskRef)`、`scheduleImmediately(TaskRef)` 存在，旧 `spawn(...)` 风格接口被移除。
 * 通过条件：编译期静态断言全部通过，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>

#include <concepts>
#include <type_traits>
#include <utility>

using namespace galay::kernel;

template <typename T>
concept HasSchedule = requires(T scheduler, TaskRef task) {
    { scheduler.schedule(std::move(task)) } -> std::same_as<bool>;
};

template <typename T>
concept HasScheduleDeferred = requires(T scheduler, TaskRef task) {
    { scheduler.scheduleDeferred(std::move(task)) } -> std::same_as<bool>;
};

template <typename T>
concept HasScheduleImmediately = requires(T scheduler, TaskRef task) {
    { scheduler.scheduleImmediately(std::move(task)) } -> std::same_as<bool>;
};

template <typename T>
concept HasSpawnTaskRef = requires(T scheduler, TaskRef task) {
    { scheduler.spawn(std::move(task)) } -> std::same_as<bool>;
};

template <typename T>
concept HasSpawnTask = requires(T scheduler, Task<void> task) {
    { scheduler.spawn(std::move(task)) } -> std::same_as<bool>;
};

static_assert(HasSchedule<Scheduler>);
static_assert(HasScheduleDeferred<Scheduler>);
static_assert(HasScheduleImmediately<Scheduler>);
static_assert(!HasSpawnTaskRef<Scheduler>);
static_assert(!HasSpawnTask<Scheduler>);

int main()
{
    return 0;
}
