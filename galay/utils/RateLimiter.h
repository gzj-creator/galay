#ifndef __GALAY_RATELIMITER_H__
#define __GALAY_RATELIMITER_H__

#include <cstdint>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace galay::utils
{
    /**
     * @brief 计数信号量
     * @details 线程安全的计数信号量实现，用于流量控制和资源限制
     */
    class CountSemaphore
    {
    public:
        using ptr = std::shared_ptr<CountSemaphore>;
        using uptr = std::unique_ptr<CountSemaphore>;
        
        /**
         * @brief 构造计数信号量
         * @param initcount 初始计数值
         * @param capacity 最大容量
         */
        CountSemaphore(uint64_t initcount, uint64_t capacity);
        
        /**
         * @brief 获取指定数量的资源
         * @param count 需要获取的资源数量
         * @return 是否成功获取（会阻塞直到资源可用）
         */
        bool get(uint64_t count);
        
        /**
         * @brief 释放指定数量的资源
         * @param count 释放的资源数量
         */
        void put(uint64_t count);
    private:
        std::mutex m_mtx;
        uint64_t m_capacity;         ///< 最大容量
        uint64_t m_nowcount;         ///< 当前计数
        std::condition_variable m_cond;
    };

    /**
     * @brief 速率限制器（令牌桶算法）
     * @details 基于令牌桶算法的流量控制，支持突发流量处理
     *          - 最大可应对 (rate + capacity) 的突发流量
     *          - deliveryIntervalMs越大：每次投放token越多，更新频率低，性能高但及时性低
     *          - deliveryIntervalMs越小：每次投放token越少，更新频率高，性能低但及时性高
     */
    class RateLimiter{
    public:
        /**
         * @brief 构造速率限制器
         * @param rate 速率（字节/秒）
         * @param capacity 令牌桶容量
         * @param deliveryInteralMs 令牌投放间隔（毫秒）
         */
        RateLimiter(uint64_t rate, uint64_t capacity, uint64_t deliveryInteralMs);
        
        /**
         * @brief 启动速率限制器
         */
        void start();
        
        /**
         * @brief 停止速率限制器
         */
        void stop();
        
        /**
         * @brief 检查流量是否通过
         * @param flow 请求的流量大小（字节）
         * @return 是否允许通过（会阻塞直到有足够令牌）
         */
        bool pass(uint64_t flow);
        
        ~RateLimiter();
    private:
        void produceToken();
    private:
        uint64_t m_deliveryInteralMs;
        uint64_t m_rate;
        bool m_runing;
        CountSemaphore::uptr m_semaphore;
        std::unique_ptr<std::thread> m_deliveryThread;
    };
}


#endif