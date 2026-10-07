/**
 * @file io_controller.hpp
 * @brief IO 事件控制器与 io_uring SQE 状态管理
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 IOController，追踪每个 fd 的 IO 状态（事件类型、awaitable 槽位、
 * sequence 所有权）。在 io_uring 模式下还管理 SQE 代追踪、multishot accept/recv 队列、
 * 以及 provided-buffer recv 数据块缓存。
 *
 * @note IOController 非线程安全；只能在所属调度器线程上访问。
 */

#ifndef GALAY_KERNEL_IOCONTROLLER_HPP
#define GALAY_KERNEL_IOCONTROLLER_HPP

#include "../common/defn.hpp"
#include "../common/error.h"
#include "../common/host.hpp"

#include <atomic>

#ifdef USE_IOURING
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#endif

namespace galay::kernel
{

class Scheduler;
struct AcceptAwaitable;
struct RecvAwaitable;
struct SendAwaitable;
struct ConnectAwaitable;
struct RecvFromAwaitable;
struct SendToAwaitable;
struct FileReadAwaitable;
struct FileWriteAwaitable;
struct FileWatchAwaitable;
struct ReadvAwaitable;
struct WritevAwaitable;
struct SendFileAwaitable;
struct SequenceAwaitableBase;

/**
 * @brief IO事件控制器
 *
 * @details 管理单个IO操作的状态和回调。
 * 每个异步IO操作都关联一个IOController。
 *
 * @note
 * - 存储当前IO操作类型和对应的Awaitable对象
 * - 支持超时机制，通过 generation 和 state 防止重复唤醒
 * - 支持同时 RECV 和 SEND
 * - !!! 不允许跨调度器执行(非线程安全)
 */
struct IOController;

#ifdef USE_IOURING
/**
 * @brief SQE 标签，生命周期与 IOController 绑定
 * @details 用作 io_uring sqe 的 user_data，避免超时销毁 awaitable 后 CQE 解引用野指针
 */
struct SqeState {
    /**
     * @brief 构造 SQE 状态对象
     * @param controller 所属的 IOController
     * @param index 对应的 READ/WRITE 槽位
     */
    explicit SqeState(IOController* controller, uint8_t index)
        : owner(controller)
        , slot(index) {}

    std::atomic<IOController*> owner;  ///< 当前拥有该 SQE 槽位的 IOController
    const uint8_t slot;  ///< READ/WRITE 槽位编号
    std::atomic<uint64_t> generation{1};  ///< 每次重绑或失效时递增，用于过滤过期 CQE
};

/**
 * @brief io_uring 请求句柄
 * @details 作为提交到 reactor 的 user_data 包装，携带共享状态和对应 generation。
 */
struct SqeHandleArena;

struct SqeRequestHandle {
    std::shared_ptr<SqeHandleArena> arena;  ///< 持有句柄池生命周期，保证晚到 CQE 仍可安全回收
    SqeState* state = nullptr;  ///< 借用的 SQE 状态；真实生命周期由 handle arena 保活
    SqeRequestHandle* next_free = nullptr;  ///< 空闲链表指针
    uint64_t generation = 0;  ///< 本次提交时观测到的 generation
    IOEventType multishot_type = IOEventType::INVALID;  ///< 持久请求的结果类型；owner 失效后仍可识别 accepted fd。
    bool persistent = false;  ///< 是否绑定到会产生多次 CQE 的持久请求
    bool notify_expected = false;  ///< 当前请求是否还在等待 zero-copy notification CQE
    bool notify_received = false;  ///< 是否已经收到 zero-copy notification CQE
    bool result_completed = false;  ///< 业务完成 CQE 是否已经处理完毕

    void recycle() noexcept;  ///< 将请求句柄归还到所属池
};

/**
 * @brief io_uring 请求句柄池
 * @details 预分配稳定地址的 handle 块，避免热路径 `new/delete`，并允许晚到 CQE 在控制器迁移后安全回收。
 */
struct SqeHandleArena {
    /**
     * @brief 构造请求句柄池
     * @details 初始化首个句柄块，保证常规热路径可直接获取 handle。
     */
    SqeHandleArena(IOController* owner, uint8_t slot)
        : m_state(owner, slot) {
        grow();
    }

    SqeHandleArena(const SqeHandleArena&) = delete;
    SqeHandleArena& operator=(const SqeHandleArena&) = delete;

    /**
     * @brief 从池中借出一个请求句柄
     * @param state 当前请求对应的共享 SQE 状态
     * @param self 当前池的共享所有权，用于让 in-flight handle 保活池对象
     * @return 成功返回稳定地址 handle；扩容失败或状态缺失时返回 nullptr
     */
    SqeRequestHandle* acquire(SqeState* state,
                             const std::shared_ptr<SqeHandleArena>& self) {
        if (state == nullptr) {
            return nullptr;
        }
        if (m_free == nullptr) {
            if (!grow()) {
                return nullptr;
            }
        }

        auto* handle = m_free;
        m_free = handle->next_free;
        handle->next_free = nullptr;
        handle->state = state;
        handle->arena = self;
        handle->generation = state->generation.load(std::memory_order_acquire);
        return handle;
    }

    /**
     * @brief 回收请求句柄
     * @param handle 待归还的 handle；允许为空
     */
    void recycle(SqeRequestHandle* handle) noexcept {
        if (handle == nullptr) {
            return;
        }
        handle->state = nullptr;
        handle->arena.reset();
        handle->generation = 0;
        handle->multishot_type = IOEventType::INVALID;
        handle->persistent = false;
        handle->notify_expected = false;
        handle->notify_received = false;
        handle->result_completed = false;
        handle->next_free = m_free;
        m_free = handle;
    }

    SqeState* state() noexcept { return &m_state; }
    const SqeState* state() const noexcept { return &m_state; }

    /**
     * @brief 返回持久 UDP recvmsg 使用的稳定 msghdr
     * @return arena 生命周期内地址稳定的 msghdr；只允许所属 READ handle 在 in-flight 期间使用
     */
    msghdr* recv_from_message() noexcept { return &m_recvfrom_message; }

private:
    struct Block {
        static constexpr size_t kHandleCount = 8;

        std::unique_ptr<Block> next;  ///< 下一块句柄存储
        SqeRequestHandle handles[kHandleCount];  ///< 稳定地址 handle 数组
    };

    bool grow() {
        auto block = std::make_unique<Block>();
        if (!block) {
            return false;
        }
        for (auto& handle : block->handles) {
            handle.next_free = m_free;
            m_free = &handle;
        }
        block->next = std::move(m_blocks);
        m_blocks = std::move(block);
        return true;
    }

    msghdr m_recvfrom_message{};  ///< UDP multishot recvmsg 的稳定参数存储
    SqeState m_state;  ///< 由 arena 保活的稳定 SQE 状态对象
    std::unique_ptr<Block> m_blocks;  ///< 所有已分配 handle 块
    SqeRequestHandle* m_free = nullptr;  ///< 空闲 handle 单链表头
};

inline void SqeRequestHandle::recycle() noexcept {
    auto keep_alive = arena;
    if (keep_alive) {
        keep_alive->recycle(this);
    }
}

/**
 * @brief recv ready queue 的单个内部 buffer 片段
 * @details 保存由 io_uring provided buffer ring 填充完成但尚未交付给用户 buffer 的数据片段。
 */
struct ReadyRecvChunk {
    enum class Kind : uint8_t {
        Buffer,  ///< 正常数据片段
        Eof,     ///< 对端关闭，下一次 recv 应返回 0
        Error    ///< 错误结果，下一次 recv 应返回对应错误
    };

    ReadyRecvChunk() = default;

    ReadyRecvChunk(ReadyRecvChunk&& other) noexcept
    {
        move_from(std::move(other));
    }

    ReadyRecvChunk& operator=(ReadyRecvChunk&& other) noexcept
    {
        if (this != &other) {
            release();
            move_from(std::move(other));
        }
        return *this;
    }

    std::shared_ptr<void> owner;  ///< 持有底层 buffer pool 生命周期
    char* data = nullptr;  ///< buffer 起始地址
    uint16_t bid = 0;  ///< provided buffer id 标识
    size_t offset = 0;  ///< 当前尚未消费数据的起始偏移
    size_t length = 0;  ///< 当前尚未消费数据长度
    Kind kind = Kind::Buffer;  ///< 片段类型
    std::expected<size_t, IOError> result = 0;  ///< EOF / Error 情况下交付给 awaitable 的结果
    void (*recycle)(const std::shared_ptr<void>&, uint16_t) noexcept = nullptr;  ///< 归还 buffer 到 ring 的回调

    void release() noexcept {
        if (recycle != nullptr && owner) {
            recycle(owner, bid);
        }
        owner.reset();
        data = nullptr;
        bid = 0;
        offset = 0;
        length = 0;
        recycle = nullptr;
    }

private:
    ReadyRecvChunk(const ReadyRecvChunk&) = delete;
    ReadyRecvChunk& operator=(const ReadyRecvChunk&) = delete;

    void move_from(ReadyRecvChunk&& other) noexcept
    {
        owner = std::move(other.owner);
        data = other.data;
        bid = other.bid;
        offset = other.offset;
        length = other.length;
        kind = other.kind;
        result = std::move(other.result);
        recycle = other.recycle;

        other.data = nullptr;
        other.bid = 0;
        other.offset = 0;
        other.length = 0;
        other.kind = Kind::Buffer;
        other.result = size_t{0};
        other.recycle = nullptr;
    }
};

/**
 * @brief recvfrom ready queue 的单个完整数据报
 * @details UDP 数据报只允许整包消费一次；用户缓冲不足时复制可容纳的前缀并丢弃余量，
 *          不得像 TCP 流一样把余量交给下一次 recvfrom。
 */
struct ReadyRecvDatagram {
    enum class Kind : uint8_t {
        Buffer,  ///< 正常数据报，允许长度为 0
        Error    ///< 错误结果
    };

    ReadyRecvDatagram() = default;

    ReadyRecvDatagram(ReadyRecvDatagram&& other) noexcept
    {
        move_from(std::move(other));
    }

    ReadyRecvDatagram& operator=(ReadyRecvDatagram&& other) noexcept
    {
        if (this != &other) {
            release();
            move_from(std::move(other));
        }
        return *this;
    }

    sockaddr_storage source{};  ///< 数据报源地址；仅 Buffer 类型有效
    std::shared_ptr<void> owner;  ///< 持有底层 UDP buffer pool 生命周期
    std::expected<size_t, IOError> result = 0;  ///< Error 类型交付给 awaitable 的错误
    void (*recycle)(const std::shared_ptr<void>&, uint16_t) noexcept = nullptr;  ///< 归还 buffer 到 ring 的回调
    char* data = nullptr;  ///< 数据报 payload 起始地址
    size_t length = 0;  ///< 当前数据报 payload 长度
    uint16_t bid = 0;  ///< provided buffer id 标识
    Kind kind = Kind::Buffer;  ///< 数据报类型

    void release() noexcept
    {
        if (recycle != nullptr && owner) {
            recycle(owner, bid);
        }
        owner.reset();
        data = nullptr;
        length = 0;
        bid = 0;
        recycle = nullptr;
    }

private:
    ReadyRecvDatagram(const ReadyRecvDatagram&) = delete;
    ReadyRecvDatagram& operator=(const ReadyRecvDatagram&) = delete;

    void move_from(ReadyRecvDatagram&& other) noexcept
    {
        source = other.source;
        owner = std::move(other.owner);
        result = std::move(other.result);
        recycle = other.recycle;
        data = other.data;
        length = other.length;
        bid = other.bid;
        kind = other.kind;

        other.owner.reset();
        other.data = nullptr;
        other.length = 0;
        other.bid = 0;
        other.kind = Kind::Buffer;
        other.result = size_t{0};
        other.recycle = nullptr;
    }
};
#endif

struct IOController {
    /**
     * @brief IO操作索引（用于数组访问）
     */
    enum Index : uint8_t {
        READ = 0,             ///< Read 操作
        WRITE = 1,            ///< Write 操作
        SIZE                  ///< 槽位数量
    };

    /**
     * @brief 构造 IO 控制器
     * @param handle 关联的底层句柄
     */
    IOController(GHandle handle)
        : m_handle(handle)
#ifdef USE_IOURING
        , m_sqe_handle_pool{
            std::make_shared<SqeHandleArena>(this, READ),
            std::make_shared<SqeHandleArena>(this, WRITE)}
        , m_sqe_state{
            nullptr,
            nullptr}
#endif
    {
#ifdef USE_IOURING
        m_sqe_state[READ] = m_sqe_handle_pool[READ]->state();
        m_sqe_state[WRITE] = m_sqe_handle_pool[WRITE]->state();
#endif
    }

    /**
     * @brief 析构 IO 控制器
     * @note 在 io_uring 模式下会主动使历史 SQE 请求失效
     */
    ~IOController() {
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
        release_registration_owner_slot();
#endif
#ifdef USE_IOURING
        clear_sqe_state();
#endif
    }

    IOController(const IOController&) = delete;
    IOController& operator=(const IOController&) = delete;

    // FIX: 删除移动构造和移动赋值
    // IOController 被 reactor 持有裸指针后，移动会导致指针失效
    IOController(IOController&&) = delete;
    IOController& operator=(IOController&&) = delete;

#if defined(USE_EPOLL) || defined(USE_KQUEUE)
    /**
     * @brief 绑定 reactor 稳定注册入口中的 controller 槽位
     * @note 仅在所属 IO 调度器线程调用；无需原子或锁。
     */
    void bind_registration_owner_slot(IOController** owner_slot) noexcept {
        if (m_registration_owner_slot == owner_slot) {
            if (owner_slot != nullptr) {
                *owner_slot = this;
            }
            return;
        }

        release_registration_owner_slot();
        if (owner_slot != nullptr) {
            auto* previous_owner = *owner_slot;
            if (previous_owner != nullptr && previous_owner != this) {
                previous_owner->m_registration_owner_slot = nullptr;
            }
            *owner_slot = this;
        }
        m_registration_owner_slot = owner_slot;
    }

    /**
     * @brief 解除 reactor 稳定注册入口对当前 controller 的引用
     */
    void release_registration_owner_slot() noexcept {
        if (m_registration_owner_slot != nullptr) {
            if (*m_registration_owner_slot == this) {
                *m_registration_owner_slot = nullptr;
            }
            m_registration_owner_slot = nullptr;
        }
    }
#endif

    /**
     * @brief 将 moved-from 对象重置到安全空状态
     * @note 供移动构造/赋值后清理源对象使用
     */
    void reset_moved_from() noexcept {
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
        release_registration_owner_slot();
#endif
        m_handle = GHandle::invalid();
        m_type = IOEventType::INVALID;
        m_awaitable[READ] = nullptr;
        m_awaitable[WRITE] = nullptr;
        m_sequence_owner[READ] = nullptr;
        m_sequence_owner[WRITE] = nullptr;
        m_sequence_interest_mask = 0;
        m_sequence_armed_mask = 0;
#ifdef USE_KQUEUE
        m_simple_armed_mask = 0;
#endif
        m_owner_scheduler.store(nullptr, std::memory_order_release);
#ifdef USE_EPOLL
        m_persistent_events = 0;
        m_registered_events = 0;
#endif
#ifdef USE_IOURING
        clear_sqe_state();
        m_sqe_handle_pool[READ] = std::make_shared<SqeHandleArena>(this, READ);
        m_sqe_handle_pool[WRITE] = std::make_shared<SqeHandleArena>(this, WRITE);
        m_sqe_state[READ] = m_sqe_handle_pool[READ]->state();
        m_sqe_state[WRITE] = m_sqe_handle_pool[WRITE]->state();
        m_ready_accepts.clear();
        m_ready_recvs.clear();
        m_ready_recvfrom.clear();
        m_accept_multishot_handle = nullptr;
        m_recv_multishot_handle = nullptr;
        m_recvfrom_multishot_handle = nullptr;
        m_accept_multishot_armed = false;
        m_recv_multishot_armed = false;
        m_recvfrom_multishot_armed = false;
        m_recv_result_assigned = false;
        m_recvfrom_result_assigned = false;
#endif
    }

#ifdef USE_IOURING
    /**
     * @brief 为指定槽位生成 io_uring 提交句柄
     * @param slot READ 或 WRITE 槽位
     * @return 成功时返回池内稳定地址请求句柄；池缺失、状态缺失或扩容失败时返回 nullptr
     */
    SqeRequestHandle* make_sqe_request(Index slot) const {
        auto* state = m_sqe_state[slot];
        const auto& arena = m_sqe_handle_pool[slot];
        if (state == nullptr || !arena) {
            return nullptr;
        }
        return arena->acquire(state, arena);
    }

    /**
     * @brief 推进指定槽位的 generation
     * @param slot READ 或 WRITE 槽位
     * @note 在替换 awaitable 或重绑 owner 时调用，用于让旧 CQE 自动失效
     */
    void advance_sqe_generation(Index slot) noexcept {
        if (auto* state = m_sqe_state[slot]; state != nullptr) {
            state->generation.fetch_add(1, std::memory_order_acq_rel);
            state->owner.store(this, std::memory_order_release);
        }
    }

    /**
     * @brief 使当前控制器上所有历史 SQE 请求失效
     */
    void invalidate_sqe_requests() noexcept {
        clear_sqe_state();
    }

    /**
     * @brief 将 accepted fd 缓存到 controller 侧队列
     * @param handle 新接受到的连接句柄
     */
    void enqueue_accepted_handle(GHandle handle) {
        m_ready_accepts.push_back(handle);
    }

    /** @brief 移交一个资源侧缓存的 accepted fd；调用者立即接管所有权。
     *  @return nullopt 表示队列为空；不涉及单次用户 accept 的完成裁决。 */
    std::optional<GHandle> take_accepted_handle() {
        if (m_ready_accepts.empty()) { return std::nullopt; }
        const GHandle handle = m_ready_accepts.front();
        m_ready_accepts.pop_front();
        return handle;
    }

    /**
     * @brief 清理 controller 内缓存但尚未交付的 accepted fd
     */
    void clear_accepted_handles() noexcept {
        while (!m_ready_accepts.empty()) {
            const GHandle handle = m_ready_accepts.front();
            m_ready_accepts.pop_front();
            if (handle != GHandle::invalid()) {
                galay_close(handle.fd);
            }
        }
    }

    /**
     * @brief 将 recv CQE 对应的内部 buffer 片段缓存到 controller 侧队列
     * @param chunk 已完成但尚未交付给用户的内部 recv 片段
     */
    void enqueue_ready_recv(ReadyRecvChunk&& chunk) {
        m_ready_recvs.push_back(std::move(chunk));
    }

    /**
     * @brief 尝试把 ready recv queue 中的一个结果交付到用户 buffer
     * @param buffer 用户传入的 recv buffer
     * @param capacity 用户 buffer 最大容量
     * @param result 输出 recv 结果
     * @return true 表示已消费一个 ready recv 结果；false 表示队列为空
     */
    bool try_consume_ready_recv(char* buffer,
                             size_t capacity,
                             std::expected<size_t, IOError>& result) {
        if (m_ready_recvs.empty()) {
            return false;
        }

        size_t total_bytes = 0;
        while (!m_ready_recvs.empty()) {
            auto& chunk = m_ready_recvs.front();
            if (chunk.kind == ReadyRecvChunk::Kind::Error ||
                chunk.kind == ReadyRecvChunk::Kind::Eof) {
                if (total_bytes == 0) {
                    result = chunk.result;
                    chunk.release();
                    m_ready_recvs.pop_front();
                } else {
                    result = total_bytes;
                }
                return true;
            }

            const size_t remaining = capacity > total_bytes ? capacity - total_bytes : 0;
            const size_t bytes = std::min(chunk.length, remaining);
            if (bytes > 0) {
                std::memcpy(buffer + total_bytes, chunk.data + chunk.offset, bytes);
            }
            chunk.offset += bytes;
            chunk.length -= bytes;
            total_bytes += bytes;

            if (chunk.length == 0) {
                chunk.release();
                m_ready_recvs.pop_front();
                if (total_bytes >= capacity) {
                    break;
                }
                continue;
            }

            if (total_bytes >= capacity) {
                break;
            }
        }

        result = total_bytes;
        return true;
    }

    /**
     * @brief 清理 controller 内缓存但尚未交付的 recv 片段
     */
    void clear_ready_recvs() noexcept {
        while (!m_ready_recvs.empty()) {
            m_ready_recvs.front().release();
            m_ready_recvs.pop_front();
        }
    }

    /**
     * @brief 将完整 UDP 数据报缓存到 controller 侧队列
     * @param datagram 已完成但尚未交付给用户的内部数据报
     */
    void enqueue_ready_recv_from(ReadyRecvDatagram&& datagram)
    {
        m_ready_recvfrom.push_back(std::move(datagram));
    }

    /**
     * @brief 尝试消费一个缓存 UDP 数据报
     * @param buffer 用户接收缓冲
     * @param capacity 用户缓冲容量
     * @param from 可选源地址输出
     * @param result 输出复制字节数或错误
     * @return true 表示消费了一个完整数据报或错误；false 表示队列为空
     */
    bool try_consume_ready_recv_from(char* buffer,
                                 size_t capacity,
                                 Host* from,
                                 std::expected<size_t, IOError>& result)
    {
        if (m_ready_recvfrom.empty()) {
            return false;
        }

        auto& datagram = m_ready_recvfrom.front();
        if (datagram.kind == ReadyRecvDatagram::Kind::Error) {
            result = datagram.result;
        } else {
            const size_t bytes = std::min(datagram.length, capacity);
            if (bytes > 0) {
                std::memcpy(buffer, datagram.data, bytes);
            }
            if (from != nullptr) {
                *from = Host::from_sock_addr(datagram.source);
            }
            result = bytes;
        }
        datagram.release();
        m_ready_recvfrom.pop_front();
        return true;
    }

    /**
     * @brief 归还所有尚未交付的 UDP provided buffers
     */
    void clear_ready_recv_from() noexcept
    {
        while (!m_ready_recvfrom.empty()) {
            m_ready_recvfrom.front().release();
            m_ready_recvfrom.pop_front();
        }
    }
#endif

    /**
     * @brief 填充Awaitable信息（支持 RECVWITHSEND 状态机）
     * @param type IO事件类型
     * @param awaitable 对应的Awaitable对象指针
     * @return true 填充成功；false 事件类型不受支持
     */
    bool fill_awaitable(IOEventType type, void* awaitable);

    /**
     * @brief 清除Awaitable信息（支持 RECVWITHSEND 状态机）
     * @param type IO事件类型
     */
    void remove_awaitable(IOEventType type);

    GHandle m_handle = GHandle::invalid();  ///< 关联的底层句柄
    IOEventType m_type = IOEventType::INVALID;  ///< 当前IO事件类型
    void* m_awaitable[IOController::SIZE] = {nullptr, nullptr};  ///< READ/WRITE 槽位上的 awaitable 指针
    SequenceAwaitableBase* m_sequence_owner[IOController::SIZE] = {nullptr, nullptr};  ///< READ/WRITE 槽位所属的 sequence awaitable
    std::atomic<Scheduler*> m_owner_scheduler{nullptr};  ///< direct C TCP I/O 绑定的 owner scheduler；C++ awaitable 不依赖该字段
#if defined(USE_EPOLL) || defined(USE_KQUEUE)
    IOController** m_registration_owner_slot = nullptr;  ///< reactor 稳定入口的 owner 槽；移动时原位重绑
#endif
    uint8_t m_sequence_interest_mask = 0;  ///< sequence 关心的 READ/WRITE 位掩码
    uint8_t m_sequence_armed_mask = 0;  ///< 已经向 reactor 注册的 READ/WRITE 位掩码
#ifdef USE_KQUEUE
    uint8_t m_simple_armed_mask = 0;  ///< kqueue 普通 awaitable 需要保留的 READ/WRITE 注册位
#endif
#ifdef USE_EPOLL
    uint32_t m_persistent_events = 0;  ///< epoll 跨 awaitable 保留的事件掩码；当前仅持久保留 EPOLLIN
    uint32_t m_registered_events = 0;  ///< epoll 已注册的事件掩码缓存
#endif
#ifdef USE_IOURING
    std::shared_ptr<SqeHandleArena> m_sqe_handle_pool[SIZE];  ///< io_uring READ/WRITE 槽位句柄池
    SqeState* m_sqe_state[SIZE] = {nullptr, nullptr};  ///< 借用 handle arena 内部的稳定 SQE 状态
    std::deque<GHandle> m_ready_accepts;  ///< listener 缓存的 accepted fd，供下一次 accept() 直接消费
    std::deque<ReadyRecvChunk> m_ready_recvs;  ///< socket 缓存的 ready recv 片段，供下一次 recv() 直接消费
    std::deque<ReadyRecvDatagram> m_ready_recvfrom;  ///< UDP socket 缓存的完整数据报
    SqeRequestHandle* m_accept_multishot_handle = nullptr;  ///< 当前 listener 持有的 multishot accept handle
    SqeRequestHandle* m_recv_multishot_handle = nullptr;  ///< 当前 socket 持有的 multishot recv handle
    SqeRequestHandle* m_recvfrom_multishot_handle = nullptr;  ///< 当前 UDP socket 持有的 multishot recvmsg handle
    bool m_accept_multishot_armed = false;  ///< listener 当前是否已挂上 multishot accept SQE
    bool m_recv_multishot_armed = false;  ///< socket 当前是否已挂上 multishot recv SQE
    bool m_recvfrom_multishot_armed = false;  ///< UDP socket 当前是否已挂上 multishot recvmsg SQE
    bool m_recv_result_assigned = false;  ///< 当前 suspended recv awaitable 是否已写入一个结果
    bool m_recvfrom_result_assigned = false;  ///< 当前 suspended recvfrom awaitable 是否已写入一个结果
#endif

    /**
     * @brief 按具体 Awaitable 类型访问当前控制器中缓存的等待体
     * @tparam T 目标 awaitable 类型
     * @return 若当前槽位存在对应 awaitable，则返回其类型化指针；默认模板返回 nullptr
     * @note 具体事件类型的显式特化定义位于本头文件末尾
     */
    template<typename T>
    T* get_awaitable() { return nullptr; }

#ifdef USE_IOURING
private:
    void clear_sqe_state() noexcept {
        clear_accepted_handles();
        clear_ready_recvs();
        clear_ready_recv_from();
        m_accept_multishot_handle = nullptr;
        m_recv_multishot_handle = nullptr;
        m_recvfrom_multishot_handle = nullptr;
        m_accept_multishot_armed = false;
        m_recv_multishot_armed = false;
        m_recvfrom_multishot_armed = false;
        m_recv_result_assigned = false;
        m_recvfrom_result_assigned = false;
        for (auto* state : m_sqe_state) {
            if (state == nullptr) {
                continue;
            }
            state->owner.store(nullptr, std::memory_order_release);
            state->generation.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void rebind_sqe_state() noexcept {
        for (auto* state : m_sqe_state) {
            if (state == nullptr) {
                continue;
            }
            state->owner.store(this, std::memory_order_release);
        }
    }
#endif

#if defined(USE_EPOLL) || defined(USE_KQUEUE)
private:
    void adopt_registration_owner_slot(IOController& other) noexcept {
        m_registration_owner_slot = other.m_registration_owner_slot;
        if (m_registration_owner_slot != nullptr) {
            if (*m_registration_owner_slot == &other) {
                *m_registration_owner_slot = this;
            } else {
                m_registration_owner_slot = nullptr;
            }
        }
        other.m_registration_owner_slot = nullptr;
    }
#endif
};

/**
 * @brief IOController::get_awaitable 的显式特化集合
 * @details 这些访问器把 READ/WRITE 槽位上的 `void*` awaitable 安全转换为具体类型。
 */

template<>
inline auto IOController::get_awaitable() -> AcceptAwaitable* {
    return static_cast<AcceptAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::get_awaitable() -> RecvAwaitable* {
    return static_cast<RecvAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::get_awaitable() -> SendAwaitable* {
    return static_cast<SendAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::get_awaitable() -> ConnectAwaitable* {
    return static_cast<ConnectAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::get_awaitable() -> RecvFromAwaitable* {
    return static_cast<RecvFromAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::get_awaitable() -> SendToAwaitable* {
    return static_cast<SendToAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::get_awaitable() -> FileReadAwaitable* {
    return static_cast<FileReadAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::get_awaitable() -> FileWriteAwaitable* {
    return static_cast<FileWriteAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::get_awaitable() -> FileWatchAwaitable* {
    return static_cast<FileWatchAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::get_awaitable() -> ReadvAwaitable* {
    return static_cast<ReadvAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::get_awaitable() -> WritevAwaitable* {
    return static_cast<WritevAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::get_awaitable() -> SendFileAwaitable* {
    return static_cast<SendFileAwaitable*>(m_awaitable[WRITE]);
}


inline bool IOController::fill_awaitable(IOEventType type, void* awaitable) {
    constexpr uint32_t kReadSlotMask =
        static_cast<uint32_t>(IOEventType::ACCEPT) |
        static_cast<uint32_t>(IOEventType::RECV) |
        static_cast<uint32_t>(IOEventType::READV) |
        static_cast<uint32_t>(IOEventType::FILEREAD) |
        static_cast<uint32_t>(IOEventType::FILEWATCH) |
        static_cast<uint32_t>(IOEventType::RECVFROM);
    constexpr uint32_t kWriteSlotMask =
        static_cast<uint32_t>(IOEventType::CONNECT) |
        static_cast<uint32_t>(IOEventType::SEND) |
        static_cast<uint32_t>(IOEventType::WRITEV) |
        static_cast<uint32_t>(IOEventType::SENDFILE) |
        static_cast<uint32_t>(IOEventType::FILEWRITE) |
        static_cast<uint32_t>(IOEventType::SENDTO);

    switch (type) {
    case IOEventType::RECV:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
#ifdef USE_IOURING
        m_recv_result_assigned = false;
#endif
        break;
    case IOEventType::READV:
    case IOEventType::FILEREAD:
    case IOEventType::FILEWATCH:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
#ifdef USE_IOURING
        advance_sqe_generation(READ);
#endif
        break;
    case IOEventType::RECVFROM:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
#ifdef USE_IOURING
        if (!m_recvfrom_multishot_armed) {
            advance_sqe_generation(READ);
        }
        m_recvfrom_result_assigned = false;
#endif
        break;
    case IOEventType::ACCEPT:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
        break;
    case IOEventType::SEQUENCE:
        m_type |= type;
#ifdef USE_IOURING
        m_awaitable[READ] = awaitable;
        advance_sqe_generation(READ);
#endif
        break;
    case IOEventType::SEND:
    case IOEventType::WRITEV:
    case IOEventType::SENDFILE:
    case IOEventType::FILEWRITE:
    case IOEventType::SENDTO:
    case IOEventType::CONNECT:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kWriteSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[WRITE] = awaitable;
#ifdef USE_IOURING
        advance_sqe_generation(WRITE);
#endif
        break;
    default:
        return false;
    }
    return true;
}

inline void IOController::remove_awaitable(IOEventType type) {
    m_type &= ~type;
    switch (type) {
    case IOEventType::RECV:
        m_awaitable[READ] = nullptr;
#ifdef USE_IOURING
        m_recv_result_assigned = false;
#endif
        break;
    case IOEventType::READV:
    case IOEventType::FILEREAD:
    case IOEventType::FILEWATCH:
        m_awaitable[READ] = nullptr;
#ifdef USE_IOURING
        advance_sqe_generation(READ);
#endif
        break;
    case IOEventType::RECVFROM:
        m_awaitable[READ] = nullptr;
#ifdef USE_IOURING
        if (!m_recvfrom_multishot_armed) {
            advance_sqe_generation(READ);
        }
        m_recvfrom_result_assigned = false;
#endif
        break;
    case IOEventType::ACCEPT:
        m_awaitable[READ] = nullptr;
        break;
    case IOEventType::SEQUENCE:
#ifdef USE_IOURING
        m_awaitable[READ] = nullptr;
        advance_sqe_generation(READ);
#endif
        break;
    case IOEventType::SEND:
    case IOEventType::WRITEV:
    case IOEventType::SENDFILE:
    case IOEventType::FILEWRITE:
    case IOEventType::SENDTO:
    case IOEventType::CONNECT:
        m_awaitable[WRITE] = nullptr;
#ifdef USE_IOURING
        advance_sqe_generation(WRITE);
#endif
        break;
    default:
        break;
    }
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_IOCONTROLLER_HPP
