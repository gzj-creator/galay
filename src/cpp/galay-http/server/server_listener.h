#ifndef GALAY_HTTP_SERVER_LISTENER_H
#define GALAY_HTTP_SERVER_LISTENER_H

#include "../common/http_log.h"
#include "../../galay-kernel/async/async_tcp.h"
#include "../../galay-kernel/async/async_waiter.h"
#include "../../galay-kernel/core/runtime.h"

#include <cerrno>
#include <expected>
#include <iterator>
#include <list>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace galay::http::server_detail {

class ServerConnections {
    struct Entry {
        int descriptor;
        kernel::AsyncWaiter<void> completed;
    };
    struct Lane {
        kernel::IOScheduler* scheduler;
        std::list<std::shared_ptr<Entry>> entries;
        bool stopping = false;
    };

public:
    class Scope {
    public:
        Scope(Scope&& other) noexcept
            : lane_(std::exchange(other.lane_, nullptr)), position_(other.position_) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope& operator=(Scope&&) = delete;
        ~Scope() { finish(); }

        void finish()
        {
            if (!lane_) return;
            auto entry = *position_;
            entry->descriptor = -1;
            // The iterator after the removed connection is not needed.
            const auto next = lane_->entries.erase(position_);
            (void)next;
            lane_ = nullptr;
            if (!entry->completed.notify()) {
                HTTP_LOG_WARN("[server] [drain-fail]", "connection completed twice");
            }
        }

        void release_handle() noexcept
        {
            if (lane_) {
                (*position_)->descriptor = -1;
            }
        }

    private:
        friend class ServerConnections;
        Scope(Lane* lane, int descriptor) : lane_(lane)
        {
            lane_->entries.push_back(std::make_shared<Entry>(descriptor));
            position_ = std::prev(lane_->entries.end());
        }
        Lane* lane_;
        std::list<std::shared_ptr<Entry>>::iterator position_;
    };

    class AttachAwaitable {
    public:
        AttachAwaitable(ServerConnections& connections, int descriptor)
            : connections_(connections), descriptor_(descriptor) {}
        bool await_ready() const noexcept { return false; }
        template<class Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            const auto& task = handle.promise().task_ref_view();
            scheduler_ = task.belong_scheduler();
            return false;
        }
        std::expected<Scope, kernel::IOError> await_resume()
        {
            if (descriptor_ < 0) {
                return std::unexpected(kernel::IOError(kernel::kParamInvalid, 0));
            }
            for (const auto& lane : connections_.lanes_) {
                if (lane->scheduler != scheduler_) continue;
                if (lane->stopping) return std::unexpected(kernel::IOError(kernel::kClosed, 0));
                return Scope(lane.get(), descriptor_);
            }
            return std::unexpected(kernel::IOError(kernel::kNotRunningOnIOScheduler, 0));
        }
    private:
        ServerConnections& connections_;
        int descriptor_;
        kernel::Scheduler* scheduler_ = nullptr;
    };

    void start(kernel::Runtime& runtime)
    {
        lanes_.clear();
        for (std::size_t index = 0; index < runtime.get_io_scheduler_count(); ++index) {
            lanes_.push_back(std::make_unique<Lane>(runtime.get_io_scheduler(index)));
        }
        started_ = true;
    }

    AttachAwaitable attach(int descriptor) { return AttachAwaitable(*this, descriptor); }

    void stop(kernel::Runtime& runtime)
    {
        if (!started_) return;
        std::vector<kernel::JoinHandle<void>> pending;
        for (const auto& lane : lanes_) {
            auto task = drain(lane.get());
            const auto& reference = kernel::detail::TaskAccess::task_ref(task);
            kernel::detail::set_task_runtime(reference, &runtime);
            kernel::detail::set_task_scheduler(reference, lane->scheduler);
            if (lane->scheduler && lane->scheduler->schedule(reference)) {
                pending.emplace_back(kernel::detail::TaskAccess::detach_task(std::move(task)));
            } else {
                HTTP_LOG_ERROR("[server] [drain-fail]", "cannot schedule connection drain");
            }
        }
        for (auto& task : pending) {
            if (const auto completed = task.join(); !completed) {
                HTTP_LOG_ERROR("[server] [drain-fail]", "error={}", completed.error().message());
            }
        }
        started_ = false;
    }

private:
    static kernel::Task<void> drain(Lane* lane)
    {
        lane->stopping = true;
        // Only the IO owner touches registrations. Keep its runtime/timers alive
        // until interrupted reads, stream handlers and their continuations finish.
        std::vector<std::shared_ptr<Entry>> entries(lane->entries.begin(), lane->entries.end());
        for (const auto& entry : entries) {
            if (entry->descriptor < 0) continue;
            int result;
            do { result = ::shutdown(entry->descriptor, SHUT_RDWR); } while (result < 0 && errno == EINTR);
            if (result < 0 && errno != ENOTCONN) {
                HTTP_LOG_WARN("[server] [shutdown-fail]", "fd={} errno={}", entry->descriptor, errno);
            }
        }
        for (const auto& entry : entries) {
            if (const auto completed = co_await entry->completed.wait(); !completed) {
                HTTP_LOG_ERROR("[server] [drain-fail]", "error={}", completed.error().message());
            }
        }
    }

    std::vector<std::unique_ptr<Lane>> lanes_;
    bool started_ = false;
};

inline std::expected<async::AsyncTcpSocket, kernel::IOError> create_listener(
    const std::string& host, std::uint16_t port, int backlog)
{
    auto listener = async::AsyncTcpSocket::create(kernel::IPType::IPV4);
    if (!listener) return std::unexpected(listener.error());
    if (const auto result = listener->option().handle_reuse_addr(); !result) return std::unexpected(result.error());
    if (const auto result = listener->option().handle_reuse_port(); !result) return std::unexpected(result.error());
    if (const auto result = listener->option().handle_non_block(); !result) return std::unexpected(result.error());
    const kernel::Host address(kernel::IPType::IPV4, host, port);
    if (const auto result = listener->bind(address); !result) return std::unexpected(result.error());
    if (const auto result = listener->listen(backlog); !result) return std::unexpected(result.error());
    return std::move(*listener);
}

inline kernel::Task<void> close_listener(async::AsyncTcpSocket* listener)
{
    if (!listener || listener->handle() == ::GHandle::invalid()) co_return;
    const auto closed = co_await listener->close();
    if (!closed && closed.error().code() != kernel::kClosed) {
        HTTP_LOG_WARN("[socket] [close-fail]", "context=server-listener error={}", closed.error().message());
    }
}

// Lifecycle operations run on the calling control thread, never in a handler.
inline void close_listeners(kernel::Runtime& runtime, std::vector<async::AsyncTcpSocket>& listeners)
{
    std::vector<kernel::JoinHandle<void>> pending;
    pending.reserve(listeners.size());
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        auto* scheduler = runtime.get_io_scheduler(index);
        auto task = close_listener(&listeners[index]);
        if (!scheduler || !task.is_valid()) {
            HTTP_LOG_WARN("[runtime] [schedule-fail]", "context=server-listener-close index={}", index);
            continue;
        }
        const auto& reference = kernel::detail::TaskAccess::task_ref(task);
        kernel::detail::set_task_runtime(reference, &runtime);
        kernel::detail::set_task_scheduler(reference, scheduler);
        if (scheduler->schedule(reference)) {
            pending.emplace_back(kernel::detail::TaskAccess::detach_task(std::move(task)));
        } else {
            HTTP_LOG_WARN("[runtime] [schedule-fail]", "context=server-listener-close index={}", index);
        }
    }
    for (const auto& task : pending) {
        if (const auto waited = task.wait(); !waited) {
            HTTP_LOG_WARN("[runtime] [wait-fail]", "context=server-listener-close");
        }
    }
}

} // namespace galay::http::server_detail

#endif
