#ifndef GALAY_COROUTINE_HPP
#define GALAY_COROUTINE_HPP

#include <memory>
#include <coroutine>
#include <optional>
#include <queue>
#include <atomic>
#include <variant>
#include <expected>
#include <functional>
#include <concepts>

namespace galay 
{

using nil = std::monostate;

template <typename T>
concept CoType = !std::is_void_v<T> && std::is_default_constructible_v<T> && std::is_move_assignable_v<T>;

template<CoType T>
class PromiseType;

template<CoType T>
class Coroutine;

template<CoType T>
class CoroutineDataVisitor;

class CoroutineScheduler;

enum class CoroutineStatus: uint8_t {
    Suspended,              // 挂起
    Waking,                 // 调度唤醒
    Running,                // 运行中
    Destroying,             // 调度销毁
    Finished,               // 完成
};


class CoroutineBase: public std::enable_shared_from_this<CoroutineBase>
{
    template<CoType T>
    friend class AsyncResult;
    friend class CoroutineScheduler;
public:
    using ptr = std::shared_ptr<CoroutineBase>;
    using wptr = std::weak_ptr<CoroutineBase>;

    using Handler = std::function<void()>;
    
    virtual bool isRunning() const = 0;
    virtual bool isSuspend() const = 0;
    virtual bool isWaking() const = 0;
    virtual bool isDestroying() const = 0;
    virtual bool isDone() const = 0;
    virtual CoroutineScheduler* belongScheduler() const = 0;
    virtual void resume() = 0;
    virtual void destroy() = 0;
    virtual CoroutineBase& then(const Handler& callback) = 0;
    virtual ~CoroutineBase() = default;

    virtual void modToSuspend() = 0;
    virtual void modToWaking() = 0;
    virtual void modToRunning() = 0;
    virtual void modToDestroying() = 0;
    virtual void modToFinished() = 0;
    
    // 原子的条件状态转换：只有当前状态为 Suspended 时才转换为 Waking
    // 返回 true 表示成功转换，false 表示当前状态不允许转换
    // actual_status 会被设置为实际的当前状态
    virtual bool tryModToWaking(CoroutineStatus& actual_status) = 0;
    
    // 原子的条件状态转换：尝试将状态转换为 Destroying
    // 可以从 Suspended 或 Waking 状态转换为 Destroying
    // 返回 true 表示成功转换，false 表示当前状态不允许转换
    // actual_status 会被设置为实际的当前状态
    virtual bool tryModToDestroying(CoroutineStatus& actual_status) = 0;

protected:
    virtual void belongScheduler(CoroutineScheduler* scheduler) = 0;
};

class PromiseTypeBase
{
public:
    virtual std::weak_ptr<CoroutineBase> getCoroutine() = 0;
    virtual ~PromiseTypeBase() = default;
};



template<CoType T>
class PromiseType: public PromiseTypeBase
{
    friend class Coroutine<T>;
    struct suspend_choice {
        bool should_suspend = true;
        bool await_ready() const noexcept { return !should_suspend; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept {}
    };
public:
    template<CoType U>
    struct YieldValue {
        U value;
        bool should_suspend = true;
    };

    int get_return_object_on_alloaction_failure() noexcept { return -1; }
    Coroutine<T> get_return_object() noexcept;
    std::suspend_always initial_suspend() noexcept { return {}; }
    suspend_choice yield_value(YieldValue<T>&& value) noexcept;
    std::suspend_never final_suspend() noexcept { return {};  }
    void unhandled_exception() noexcept {}
    void return_value (T&& value) const noexcept;

    std::weak_ptr<CoroutineBase> getCoroutine() override { return m_coroutine; }

    ~PromiseType();
private:
    std::shared_ptr<Coroutine<T>> m_coroutine;
};


/*
Exit:
    status -> Finished
        |
    defer stack
*/

template<CoType T>
class Coroutine: public CoroutineBase
{
    friend class PromiseType<T>;
    friend class CoroutineScheduler;
    friend class CoroutineConsumer;
    template<CoType B>
    friend class CoroutineDataVisitor; 

    struct alignas(64) CoroutineData
    {
        std::optional<T> m_result;
        //多线程访问
        std::atomic<CoroutineStatus> m_status = CoroutineStatus::Suspended;
        std::atomic<CoroutineScheduler*> m_scheduler = nullptr;

        std::queue<Handler> m_cbs;
    };

public:
    using ptr = std::shared_ptr<Coroutine>;
    using wptr = std::weak_ptr<Coroutine>;
    using promise_type = PromiseType<T>;
    
    explicit Coroutine(std::coroutine_handle<promise_type> handle) noexcept;
    Coroutine(Coroutine&& other) noexcept;
    Coroutine(const Coroutine& other) noexcept;
    Coroutine& operator=(Coroutine&& other) noexcept;
    Coroutine& operator=(const Coroutine& other) noexcept;
    
    CoroutineScheduler* belongScheduler() const override;

    bool isRunning() const override;
    bool isSuspend() const override;
    bool isWaking() const override;
    bool isDestroying() const override;
    bool isDone() const override;

    void destroy() override;
    void resume() override;
    CoroutineBase& then(const Handler& callback) override;
    
    std::optional<T> result();
    std::optional<T> operator()();

    CoroutineBase::wptr origin();
    ~Coroutine() override = default;
private:
    void modToSuspend() override {
        return m_data->m_status.store(CoroutineStatus::Suspended, std::memory_order_relaxed);
    }

    void modToWaking() override {
        return m_data->m_status.store(CoroutineStatus::Waking, std::memory_order_relaxed);
    }

    void modToRunning() override {
        m_data->m_status.store(CoroutineStatus::Running, std::memory_order_relaxed);
    }

    void modToDestroying() override {
        m_data->m_status.store(CoroutineStatus::Destroying, std::memory_order_relaxed);
    }

    void modToFinished() override {
        m_data->m_status.store(CoroutineStatus::Finished, std::memory_order_relaxed);
    }
    
    bool tryModToWaking(CoroutineStatus& actual_status) override {
        actual_status = CoroutineStatus::Suspended;
        return m_data->m_status.compare_exchange_strong(
            actual_status,
            CoroutineStatus::Waking,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    }
    
    bool tryModToDestroying(CoroutineStatus& actual_status) override {
        // 尝试从 Suspended 转换为 Destroying
        actual_status = CoroutineStatus::Suspended;
        if (m_data->m_status.compare_exchange_strong(
                actual_status,
                CoroutineStatus::Destroying,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }

        // 如果失败，可能是 Waking 状态，尝试从 Waking 转换为 Destroying
        // 这是处理竞态的关键：即使协程已经在调度队列中，也能将其标记为销毁
        if (actual_status == CoroutineStatus::Waking) {
            if (m_data->m_status.compare_exchange_strong(
                    actual_status,
                    CoroutineStatus::Destroying,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }

        // 转换失败，actual_status 包含实际状态
        return false;
    }

    void belongScheduler(CoroutineScheduler* scheduler) override;
    void executeDeferTask();
private:
    std::coroutine_handle<promise_type> m_handle;
    std::shared_ptr<CoroutineData> m_data;
};


template<CoType T>
class CoroutineDataVisitor {
public:
    CoroutineDataVisitor(CoroutineBase::wptr coroutine);
    bool setResult(T&& result);
    bool setScheduler(CoroutineScheduler* scheduler);
private:
    CoroutineBase::wptr m_coroutine;
};




}

#include "Coroutine.inl"

#endif
