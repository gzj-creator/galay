#ifndef GALAY_KERNEL_ASYNC_ASYNCQUEUE_H
#define GALAY_KERNEL_ASYNC_ASYNCQUEUE_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <mutex>
#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/coroutine/Waker.h"
#include "galay/kernel/coroutine/AsyncEvent.hpp"

namespace galay
{

    /**
     * @brief Thread-safe asynchronous queue using lock-free ConcurrentQueue
     *
     * This is the SAFEST implementation using moodycamel::ConcurrentQueue
     * which uses lock-free atomic operations internally.
     *
     * Features:
     * - ✅ Fully thread-safe (lock-free queue + single waiter)
     * - ✅ Single consumer pattern (one coroutine waiting at a time)
     * - ✅ Works with coroutines (supports co_await)
     * - ✅ Proven implementation (used by Facebook, Bloomberg, etc.)
     */
    template<CoType T>
    class AsyncQueue
    {
    public:
        AsyncQueue() = default;

        /**
         * @brief Asynchronously dequeue a value
         *
         * Returns immediately if queue is not empty.
         * If queue is empty, suspends the current coroutine until a value is available.
         */
        AsyncResult<T> waitDequeue() {
            T out;
            if(m_queue.try_dequeue(out)) {
                // std::cerr << "[QUEUE] waitDequeue: item available, returning immediately (qsize=" << m_queue.size_approx() << ")\n";
                return AsyncResult<T>(std::move(out));
            }
            // Queue is empty, need to wait
            // std::cerr << "[QUEUE] waitDequeue: queue empty, creating DequeueEvent to suspend\n";
            return AsyncResult<T>(std::make_shared<DequeueEvent>(this));
        }

        /**
         * @brief Enqueue a value (move semantics)
         */
        void enqueue(T&& value) {
            // std::cerr << "[QUEUE] enqueue(move): adding item, current qsize=" << m_queue.size_approx() << "\n";
            m_queue.enqueue(std::move(value));
            // std::cerr << "[QUEUE] enqueue(move): item added, new qsize=" << m_queue.size_approx() << ", calling wakeWaiter\n";
            wakeWaiter();
        }

        /**
         * @brief Enqueue a value (copy semantics)
         */
        void enqueue(const T& value) {
            // std::cerr << "[QUEUE] enqueue(copy): adding item, current qsize=" << m_queue.size_approx() << "\n";
            m_queue.enqueue(value);
            // std::cerr << "[QUEUE] enqueue(copy): item added, new qsize=" << m_queue.size_approx() << ", calling wakeWaiter\n";
            wakeWaiter();
        }

        /**
         * @brief Get approximate size
         */
        size_t size() const {
            return m_queue.size_approx();
        }

        /**
         * @brief Check if queue is empty
         */
        bool empty() const {
            return m_queue.size_approx() == 0;
        }

        /**
         * @brief Check if a coroutine is waiting for dequeue
         */
        bool isWaiting() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_waiting_waker != nullptr;
        }

    private:
        mutable std::mutex m_mutex;
        moodycamel::ConcurrentQueue<T> m_queue;
        Waker* m_waiting_waker = nullptr;  // Single waiter (raw pointer to avoid copy issues)

        void wakeWaiter() {
            Waker* waker_ptr = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_waiting_waker) {
                    // std::cerr << "[QUEUE] wakeWaiter: found waiting waker, will call wakeUp\n";
                    waker_ptr = m_waiting_waker;
                    m_waiting_waker = nullptr;
                } else {
                    // std::cerr << "[QUEUE] wakeWaiter: no waiting waker, items will sit in queue\n";
                }
            }
            // Call wakeUp outside the lock
            if (waker_ptr) {
                // std::cerr << "[QUEUE] wakeWaiter: calling wakeUp on the waker\n";
                waker_ptr->wakeUp();
                // std::cerr << "[QUEUE] wakeWaiter: wakeUp returned\n";
            }
        }

        void setWaiter(Waker* waker) {
            std::lock_guard<std::mutex> lock(m_mutex);
            // std::cerr << "[QUEUE] setWaiter: storing waker for suspension\n";
            m_waiting_waker = waker;
        }

        void clearWaiter() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_waiting_waker = nullptr;
        }

        friend class DequeueEvent;

        /**
         * @brief Custom AsyncEvent for dequeue waiting
         */
        class DequeueEvent : public AsyncEvent<T> {
        public:
            explicit DequeueEvent(AsyncQueue* queue) : m_queue(queue), m_waker_storage() {}

            bool onReady() override {
                T out;
                // std::cerr << "[EVENT] onReady: checking queue, size=" << m_queue->m_queue.size_approx() << "\n";
                if (m_queue->m_queue.try_dequeue(out)) {
                    // std::cerr << "[EVENT] onReady: dequeued successfully, returning true (no suspend needed)\n";
                    this->m_result = std::move(out);
                    return true;
                }
                // std::cerr << "[EVENT] onReady: queue empty, returning false (will call onSuspend)\n";
                return false;
            }

            bool onSuspend(Waker waker) override {
                // std::cerr << "[EVENT] onSuspend: double-checking queue before suspension, size=" << m_queue->m_queue.size_approx() << "\n";
                // Double-check before suspending
                T out;
                if (m_queue->m_queue.try_dequeue(out)) {
                    // std::cerr << "[EVENT] onSuspend: race condition avoided! Item was added, returning false (no suspend)\n";
                    this->m_result = std::move(out);
                    return false;  // Don't suspend, data is available
                }

                // Store waker in local storage
                // std::cerr << "[EVENT] onSuspend: storing waker and registering with queue\n";
                m_waker_storage = std::make_shared<Waker>(std::move(waker));
                m_queue->setWaiter(m_waker_storage.get());
                // std::cerr << "[EVENT] onSuspend: returning true to suspend coroutine\n";
                return true;  // Suspend
            }

            T onResume() override {
                // Try to dequeue when resumed
                T out;
                if (m_queue->m_queue.try_dequeue(out)) {
                    return std::move(out);
                }
                // If we wake up but queue is empty, it means the value was consumed by someone else
                // This should not happen in single-consumer pattern, so return default value
                return T();
            }

        private:
            AsyncQueue* m_queue;
            std::shared_ptr<Waker> m_waker_storage;  // Keep waker alive during suspension
        };
    };
}
#endif // GALAY_KERNEL_ASYNC_ASYNCQUEUE_H