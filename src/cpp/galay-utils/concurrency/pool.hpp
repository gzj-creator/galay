/**
 * @file pool.hpp
 * @brief 对象池实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供泛型对象池（ObjectPool）和阻塞对象池（BlockingObjectPool），
 *          支持 RAII 自动归还、自定义创建/销毁函数和容量限制。
 */

#ifndef GALAY_UTILS_POOL_HPP
#define GALAY_UTILS_POOL_HPP

#include "../common/defn.hpp"
#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <utility>

namespace galay::utils {

/**
 * @brief 可池化对象基类
 */
class PoolableObject {
public:
    virtual ~PoolableObject() = default;

    /**
     * @brief 重置对象状态，归还对象池时自动调用
     */
    virtual void reset() {}
};

/**
 * @brief 可池化对象概念约束
 * @tparam T 待检查的类型
 */
template<typename T>
concept IsPoolable = std::is_base_of_v<PoolableObject, T> || requires(T t) {
    { t.reset() } -> std::same_as<void>;
};

/**
 * @brief 泛型对象池（RAII 支持）
 * @details 提供对象的获取、归还、容量限制和收缩功能。支持自定义创建和销毁函数。
 * @tparam T 池化对象类型
 */
template<typename T>
class ObjectPool {
public:
    using Ptr = std::unique_ptr<T, std::function<void(T*)>>; ///< RAII 智能指针类型
    using Creator = std::function<T*()>; ///< 对象创建函数类型
    using Destroyer = std::function<void(T*)>; ///< 对象销毁函数类型

    /**
     * @brief 构造对象池
     * @param initialSize 初始预创建的对象数量
     * @param maxSize 最大池容量（0 表示无限制）
     * @param creator 自定义创建函数
     * @param destroyer 自定义销毁函数
     */
    explicit ObjectPool(size_t initialSize = 0,
                        size_t maxSize = 0,
                        Creator creator = nullptr,
                        Destroyer destroyer = nullptr)
        : m_state(std::make_shared<State>(
              maxSize,
              creator ? std::move(creator) : []() { return new T(); },
              destroyer ? std::move(destroyer) : [](T* p) { delete p; })) {
        for (size_t i = 0; i < initialSize; ++i) {
            m_state->pool.push(m_state->creator());
            ++m_state->totalCreated;
            ++m_state->poolSize;
        }
    }

    ~ObjectPool() {
        m_state->clear();
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    Ptr acquire() {
        T* obj = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (!m_state->pool.empty()) {
                obj = m_state->pool.front();
                m_state->pool.pop();
                --m_state->poolSize;
            }
        }

        if (!obj) {
            obj = m_state->creator();
            ++m_state->totalCreated;
        }

        auto state = m_state;
        return Ptr(obj, [state](T* p) {
            if (p) {
                if constexpr (IsPoolable<T>) {
                    p->reset();
                }
                state->release(p);
            }
        });
    }

    /**
     * @brief 尝试获取对象（池为空时返回 nullptr）
     * @return RAII 包装的对象指针，或 nullptr
     */
    Ptr tryAcquire() {
        T* obj = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (!m_state->pool.empty()) {
                obj = m_state->pool.front();
                m_state->pool.pop();
                --m_state->poolSize;
            }
        }

        if (!obj) {
            return nullptr;
        }

        auto state = m_state;
        return Ptr(obj, [state](T* p) {
            if (p) {
                if constexpr (IsPoolable<T>) {
                    p->reset();
                }
                state->release(p);
            }
        });
    }

    /**
     * @brief 获取池中可用对象数量
     * @return 可用对象数量
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->pool.size();
    }

    /**
     * @brief 获取总共创建的对象数量
     * @return 总创建数量
     */
    size_t totalCreated() const {
        return m_state->totalCreated.load();
    }

    /**
     * @brief 判断池是否为空
     * @return 为空返回 true
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->pool.empty();
    }

    /**
     * @brief 清空对象池
     */
    void clear() {
        m_state->clear();
    }

    /**
     * @brief 将池收缩到指定大小
     * @param targetSize 目标大小
     */
    void shrink(size_t targetSize) {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        while (m_state->pool.size() > targetSize) {
            m_state->destroyer(m_state->pool.front());
            m_state->pool.pop();
            --m_state->poolSize;
        }
    }

private:
    struct State {
        State(size_t max_size, Creator create, Destroyer destroy)
            : maxSize(max_size)
            , creator(std::move(create))
            , destroyer(std::move(destroy)) {}

        ~State() {
            clear();
        }

        void clear() {
            std::lock_guard<std::mutex> lock(mutex);
            while (!pool.empty()) {
                destroyer(pool.front());
                pool.pop();
            }
            poolSize = 0;
        }

        void release(T* obj) {
            std::lock_guard<std::mutex> lock(mutex);

            if (maxSize == 0 || pool.size() < maxSize) {
                pool.push(obj);
                ++poolSize;
            } else {
                destroyer(obj);
            }
        }

        mutable std::mutex mutex;
        std::queue<T*> pool;
        size_t maxSize;
        Creator creator;
        Destroyer destroyer;
        std::atomic<size_t> totalCreated{0};
        std::atomic<size_t> poolSize{0};
    };

    std::shared_ptr<State> m_state;
};

/**
 * @brief 线程安全阻塞对象池
 * @details 当池为空时，获取操作会阻塞直到有对象归还。不适合在协程中使用。
 * @tparam T 池化对象类型
 */
template<typename T>
class BlockingObjectPool {
public:
    using Ptr = std::unique_ptr<T, std::function<void(T*)>>; ///< RAII 智能指针类型
    using Creator = std::function<T*()>; ///< 对象创建函数类型
    using Destroyer = std::function<void(T*)>; ///< 对象销毁函数类型

    explicit BlockingObjectPool(size_t poolSize,
                                Creator creator = nullptr,
                                Destroyer destroyer = nullptr)
        : m_creator(creator ? std::move(creator) : []() { return new T(); })
        , m_destroyer(destroyer ? std::move(destroyer) : [](T* p) { delete p; }) {
        for (size_t i = 0; i < poolSize; ++i) {
            m_pool.push(m_creator());
        }
    }

    ~BlockingObjectPool() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_pool.empty()) {
            m_destroyer(m_pool.front());
            m_pool.pop();
        }
    }

    BlockingObjectPool(const BlockingObjectPool&) = delete;
    BlockingObjectPool& operator=(const BlockingObjectPool&) = delete;

    /**
     * @brief 阻塞获取对象，池为空时等待
     * @warning 会阻塞当前线程，不要在协程中使用
     * @return RAII 包装的对象指针
     */
    Ptr acquire() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_pool.empty(); });

        T* obj = m_pool.front();
        m_pool.pop();

        return Ptr(obj, [this](T* p) {
            if (p) {
                if constexpr (IsPoolable<T>) {
                    p->reset();
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_pool.push(p);
                }
                m_cv.notify_one();
            }
        });
    }

    /**
     * @brief 带超时的阻塞获取
     * @warning 会阻塞当前线程，不要在协程中使用
     * @tparam Rep 时钟周期类型
     * @tparam Period 时钟周期单位
     * @param timeout 超时时间
     * @return RAII 包装的对象指针，超时返回 nullptr
     */
    template<typename Rep, typename Period>
    Ptr tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_pool.empty(); })) {
            return nullptr;
        }

        T* obj = m_pool.front();
        m_pool.pop();

        return Ptr(obj, [this](T* p) {
            if (p) {
                if constexpr (IsPoolable<T>) {
                    p->reset();
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_pool.push(p);
                }
                m_cv.notify_one();
            }
        });
    }

    /**
     * @brief 获取可用对象数量
     * @return 可用对象数量
     */
    size_t available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pool.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<T*> m_pool;
    Creator m_creator;
    Destroyer m_destroyer;
};

} // namespace galay::utils

#endif // GALAY_UTILS_POOL_HPP
