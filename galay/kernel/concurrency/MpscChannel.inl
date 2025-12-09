#ifndef GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL
#define GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL

#include "MpscChannel.h"

namespace galay::mpsc
{
    namespace details {
        template<typename T>
        inline bool ChannelResult<T>::await_ready() {
            return m_channel.m_size.load(std::memory_order_acquire) > 0;
        }

        template<typename T>
        inline bool ChannelResult<T>::await_suspend(std::coroutine_handle<> handle) {
            auto wait_co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
            if (m_channel.m_size.load(std::memory_order_acquire) > 0) {
                return false;
            }
            if(auto co_ptr = wait_co.lock()) {
                m_channel.m_waker = Waker(wait_co);
                co_ptr->modToSuspend();
                return true;
            }
            return false;
        }

        template<typename T>
        inline std::optional<T> ChannelResult<T>::await_resume() const {
            auto co = m_channel.m_waker.getCoroutine();
            if(auto co_ptr = co.lock()) {
                co_ptr->modToRunning();
            }
            T value;
            if (!m_channel.m_queue.try_dequeue(value)) {
                return std::nullopt;
            }
            m_channel.m_size.fetch_sub(1, std::memory_order_acq_rel);
            return value;
        }

        template<typename T>
        inline bool BatchChannelResult<T>::await_ready() {
            return m_channel.m_size.load(std::memory_order_acquire) > 0;
        }

        template<typename T>
        inline bool BatchChannelResult<T>::await_suspend(std::coroutine_handle<> handle) {
            auto wait_co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
            if (m_channel.m_size.load(std::memory_order_acquire) > 0) {
                return false;
            }
            if(auto co_ptr = wait_co.lock()) {
                co_ptr->modToSuspend();
                m_channel.m_waker = Waker(wait_co);
                return true;
            }
            return false;
        }

        template<typename T>
        inline std::optional<std::vector<T>> BatchChannelResult<T>::await_resume() const {
            auto co = m_channel.m_waker.getCoroutine();
            if(auto co_ptr = co.lock()) {
                co_ptr->modToRunning();
            }
            std::vector<T> values(AsyncChannel<T>::BATCH_SIZE);
            size_t count = m_channel.m_queue.try_dequeue_bulk(values.data(), values.size());
            if (count == 0) {
                return std::nullopt;
            }
            // 直接减少 m_size
            // enqueue_bulk(1000) 会 fetch_add(1000)，所以正常情况下 m_size >= count
            // 如果 count > m_size，说明 m_size 和队列不同步，但这是正常的（m_size 只是近似值）
            // 使用饱和减法避免下溢：如果 count > m_size，将 m_size 设置为 0
            uint32_t current_size = m_channel.m_size.load(std::memory_order_acquire);
            if (count > current_size) {
                // 实际取出的数量超过了 m_size，说明 m_size 和队列不同步
                // 使用饱和减法：将 m_size 设置为 0（因为已经取出了所有元素）
                m_channel.m_size.store(0, std::memory_order_release);
            } else {
                m_channel.m_size.fetch_sub(count, std::memory_order_acq_rel);
            }
            values.resize(count);
            return std::move(values);
        }
    }
}
#endif