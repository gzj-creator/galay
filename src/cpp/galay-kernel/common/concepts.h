/**
 * @file concepts.h
 * @brief galay-kernel 中使用的 C++20 概念
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义模板参数的编译期约束：
 * - ChronoDuration：匹配 std::chrono::duration 类型（纳秒、毫秒等）
 * - Awaitable：匹配具有 await_ready/await_resume 的类型（基本协程等待器）
 * - AwaitableWith：匹配同时支持带特定 Promise 的 await_suspend 的 Awaitable 类型
 */

#ifndef GALAY_KERNEL_CONCEPTS_H
#define GALAY_KERNEL_CONCEPTS_H

#include <chrono>
#include <concepts>
#include <coroutine>
#include <type_traits>

namespace galay::kernel::concepts
{

/**
 * @brief 匹配 std::chrono::duration 类型的概念
 * @tparam T 要检查的类型
 * @details 当 T 是 std::chrono::duration 的特化时满足
 *          （如 std::chrono::nanoseconds、std::chrono::milliseconds）。
 */
template <typename T>
concept ChronoDuration =
    requires {
        typename std::remove_cvref_t<T>::rep;
        typename std::remove_cvref_t<T>::period;
    } &&
    std::same_as<
        std::remove_cvref_t<T>,
        std::chrono::duration<
            typename std::remove_cvref_t<T>::rep,
            typename std::remove_cvref_t<T>::period>>;

/**
 * @brief 匹配基本协程等待器的概念
 * @tparam T 要检查的类型
 * @details 当 T 可移动且提供返回 bool 的 await_ready()
 *          和 await_resume() 时满足。这是 co_await 的最低接口要求。
 */
template <typename T>
concept Awaitable = std::movable<T> && requires(T t) {
    { t.await_ready() } -> std::convertible_to<bool>;
    t.await_resume();
};

/**
 * @brief 匹配能挂起特定 promise 类型的等待器的概念
 * @tparam T 等待器类型
 * @tparam Promise 协程 promise 类型
 * @details 扩展 Awaitable，额外要求 await_suspend(coroutine_handle<Promise>)。
 *          当等待器需要访问 promise（例如存储结果）时使用。
 */
template <typename T, typename Promise>
concept AwaitableWith = Awaitable<T> && requires(T t, std::coroutine_handle<Promise> handle) {
    t.await_suspend(handle);
};

/**
 * @brief 匹配恒不挂起的等待器的概念
 * @tparam T 要检查的类型
 * @details 当 T 声明 `static constexpr bool kAlwaysReady = true;` 时满足。
 *          恒不挂起的等待器不可能发生超时注入（ready 路径不触发注入），
 *          因此即使没有注入通道也允许搭配 `.timeout()` 使用。
 */
template <typename T>
concept AlwaysReadyAwaitable =
    requires {
        { T::kAlwaysReady } -> std::convertible_to<bool>;
    } && bool{T::kAlwaysReady};

/**
 * @brief 显式超时结果注入定制点。
 *
 * `setTimeout()` is useful for awaitables whose result is kept in private
 * state, where exposing an `m_result` member would weaken the abstraction.
 */
template <typename T>
concept TimeoutSettable = requires(T& value) {
    value.setTimeout();
};

/**
 * @brief awaitable exposes a direct timeout marker.
 */
template <typename T>
concept TimeoutMarkable = requires(T& value) {
    value.markTimeout();
};

/**
 * @brief 匹配可注入超时结果的等待器的概念
 * @tparam T 要检查的类型
 * @details 满足以下任一通道即认为可注入超时：
 *          - 提供 `markTimeout()`（推荐定制点）；
 *          - 提供 `setTimeout()`（私有结果存储的显式定制点）；
 *          - 提供公共 `m_result` 成员（历史兼容，超时错误直接写入）；
 *          - 恒不挂起（kAlwaysReady，超时注入不可达）。
 *          不满足任何通道的类型与 `.timeout()` 组合会在编译期报错，
 *          而不是静默返回未定义结果。
 */
template <typename T>
concept TimeoutInjectable =
    TimeoutMarkable<T> || TimeoutSettable<T> || requires(T& t) {
        t.m_result;
    } || AlwaysReadyAwaitable<T>;

} // namespace galay::kernel::concepts

#endif // GALAY_KERNEL_CONCEPTS_H
