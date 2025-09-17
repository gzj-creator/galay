#ifndef GALAY_COROUTINE_HPP
#define GALAY_COROUTINE_HPP

#include <memory>
#include <coroutine>
#include <stack>
#include <atomic>
#include <variant>
#include <optional>
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

enum class CoroutineStatus: int32_t {
    Running = 0,
    Suspended,
    Finished,
};

class CoroutineBase: public std::enable_shared_from_this<CoroutineBase>
{
    template<CoType T>
    friend class AsyncResult;
    friend class CoroutineScheduler;
public:
    using ptr = std::shared_ptr<CoroutineBase>;
    using wptr = std::weak_ptr<CoroutineBase>;

    using ExitHandle = std::function<void(CoroutineBase::wptr)>;
    
    virtual bool isRunning() const = 0;
    virtual bool isSuspend() const = 0;
    virtual bool isDone() const = 0;
    virtual CoroutineScheduler* belongScheduler() const = 0;
    virtual void onResume() = 0;
    virtual void destroy() = 0;
    virtual ~CoroutineBase() = default;

    template<typename CoRtn>
    std::shared_ptr<Coroutine<CoRtn>> ImplCast()
    {
        return std::static_pointer_cast<Coroutine<CoRtn>>(shared_from_this());
    }
protected:
    virtual void appendExitCallback(const ExitHandle& callback) = 0;
    virtual bool become(CoroutineStatus status) = 0;
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
public:
    int get_return_object_on_alloaction_failure() noexcept { return -1; }
    Coroutine<T> get_return_object() noexcept;
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always yield_value(T&& value) noexcept;
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

    struct CoroutineData
    {
        std::optional<T> m_result;
        //多线程访问
        std::atomic<CoroutineStatus> m_status = CoroutineStatus::Running;
        std::atomic<CoroutineScheduler*> m_scheduler = nullptr;

        std::stack<ExitHandle> m_defer_stk;
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
    bool isDone() const override;

    void destroy() override;
    void onResume() override;
    
    std::optional<T> result();
    std::optional<T> operator()();

    CoroutineBase::wptr getOriginCoroutine();
    ~Coroutine() override = default;
private:
    void appendExitCallback(const ExitHandle& callback) override;
    bool become(CoroutineStatus status) override;
    void belongScheduler(CoroutineScheduler* scheduler) override;
    void exitToExecuteDeferStk();
private:
    std::coroutine_handle<promise_type> m_handle;
    std::shared_ptr<CoroutineData> m_data;
};


template<CoType T>
class CoroutineDataVisitor {
public:
    CoroutineDataVisitor(CoroutineBase::wptr coroutine);
    bool setResult(T&& result);
    bool setStatus(CoroutineStatus status);
    bool setScheduler(CoroutineScheduler* scheduler);
private:
    CoroutineBase::wptr m_coroutine;
};




}

#include "Coroutine.inl"

#endif
