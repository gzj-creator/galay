/**
 * @file waker.h
 * @brief Task<void> wake-up handle
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details Defines Waker, a lightweight handle that holds a TaskRef and can
 * request the task's scheduler to resume the associated coroutine. Used by
 * IO completion callbacks, timeouts, and other async events to re-schedule
 * a suspended coroutine.
 */

#ifndef GALAY_KERNEL_WAKER_H
#define GALAY_KERNEL_WAKER_H

#include <concepts>
#include "task.h"

namespace galay::kernel
{

namespace detail {

/**
 * @brief C 协程恢复 token 的 hook 表。
 * @details ResumeToken 是“可请求恢复”的停泊引用，不是已经入队的 ReadyEntry。
 *          C 协程实现必须提供 retain/release，使 Waker 继续保持可拷贝语义。
 */
struct ResumeTokenHooks
{
    Scheduler* (*owner_scheduler)(void* state) noexcept = nullptr;  ///< 返回 owner scheduler
    bool (*request_resume)(void* state) noexcept = nullptr;  ///< 请求将协程重新放入 ready 队列
    void (*retain)(void* state) noexcept = nullptr;  ///< 复制 token 时增加停泊引用
    void (*release)(void* state) noexcept = nullptr;  ///< 销毁 token 时释放停泊引用
};

/**
 * @brief C 协程 ResumeToken 状态首字段。
 * @note 该结构不拥有 hooks；hooks 应指向静态生命周期表。
 */
struct ResumeTokenHeader
{
    const ResumeTokenHooks* hooks = nullptr;
};

/**
 * @brief Waker 内部使用的语言中立恢复 token。
 * @details C++ 分支保存 TaskRef 并复用 requestTaskResume() 的 queued 合并语义；
 *          C 分支保存以 ResumeTokenHeader 开头的状态指针，通过 hook retain/release/request。
 */
class ResumeToken
{
public:
    ResumeToken() noexcept = default;
    explicit ResumeToken(TaskRef task) noexcept;
    static ResumeToken fromCCoroutine(void* state) noexcept;

    ResumeToken(const ResumeToken& other) noexcept;
    ResumeToken(ResumeToken&& other) noexcept;
    ~ResumeToken();

    ResumeToken& operator=(const ResumeToken& other) noexcept;
    ResumeToken& operator=(ResumeToken&& other) noexcept;

    bool isValid() const noexcept;
    Scheduler* scheduler() const noexcept;
    bool requestResume() noexcept;

private:
    enum class Kind : uintptr_t {
        Empty,
        CppTask,
        CCoroutine,
    };
    static constexpr uintptr_t kKindMask = 0x3U;

    void copyFrom(const ResumeToken& other) noexcept;
    void release() noexcept;
    void* state() const noexcept;
    Kind kind() const noexcept;
    const ResumeTokenHooks* coroHooks() const noexcept;
    static uintptr_t encode(Kind kind, void* state) noexcept;

    uintptr_t m_encoded = 0;
};

}  // namespace detail

/**
 * @brief 任务唤醒器
 * @details 持有语言中立 ResumeToken，可在其他线程或回调中把关联协程重新交回所属调度器。
 */
class Waker
{
public:
    Waker() = default;  ///< 构造空 waker
    explicit Waker(TaskRef task) noexcept;  ///< 以任务引用构造唤醒器
    explicit Waker(detail::ResumeToken token) noexcept;  ///< 以语言中立恢复 token 构造唤醒器
    template <typename Promise>
    requires requires(const Promise& promise) {
        { promise.taskRefView() } -> std::same_as<const TaskRef&>;
    }
    explicit Waker(std::coroutine_handle<Promise> handle) noexcept
        : m_token(detail::ResumeToken(handle.promise().taskRefView()))
    {
    }
    Waker(const Waker& other) = default;  ///< 拷贝唤醒器，底层共享同一恢复 token
    Waker(Waker&& waker) noexcept = default;  ///< 移动唤醒器
    Waker& operator=(const Waker& other) = default;  ///< 拷贝赋值唤醒器
    Waker& operator=(Waker&& other) noexcept = default;  ///< 移动赋值唤醒器

    Scheduler* getScheduler();  ///< 返回关联任务的所属调度器；无任务时返回 nullptr

    void wakeUp();  ///< 请求恢复关联任务；若任务无效或调度器不可用则静默忽略

private:
    detail::ResumeToken m_token;  ///< 被唤醒的目标 token
};



} // namespace galay::kernel

#endif // GALAY_KERNEL_WAKER_H
