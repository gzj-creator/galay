/** @brief AsyncFile 移动保持控制器地址稳定，并正确转移 fd 所有权。 */
#include <galay/cpp/galay-kernel/async/async_file.h>

#if defined(USE_IOURING) || defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/scheduler_dispatch.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <unistd.h>

using namespace galay::kernel;
using galay::async::AsyncFile;
using galay::async::FileOpenMode;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kContent = "stable controller";

static_assert(!std::is_move_constructible_v<IOController>);
static_assert(!std::is_move_assignable_v<IOController>);
static_assert(std::is_nothrow_move_constructible_v<AsyncFile>);
static_assert(std::is_nothrow_move_assignable_v<AsyncFile>);

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T193] " << message << '\n';
        std::abort();
    }
}

bool is_closed(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

AsyncFile make_file() {
    char path[] = "/tmp/galay_async_file_move_XXXXXX";
    const int fd = ::mkstemp(path);
    require(fd >= 0, "create temporary file");
    require(::unlink(path) == 0, "unlink owned temporary file");
    require(::pwrite(fd, kContent.data(), kContent.size(), 0) ==
                static_cast<ssize_t>(kContent.size()), "write temporary file");
    AsyncFile file;
    file.adopt(fd);
    return file;
}

void check_ownership() {
    int transferred_fd = -1;
    {
        AsyncFile destination;
        {
            auto source = make_file();
            transferred_fd = source.handle().fd;
            auto* controller = source.get_controller();
            AsyncFile moved(std::move(source));
            require(moved.get_controller() == controller, "move construction preserves controller address");
            require(moved.handle().fd == transferred_fd, "move construction transfers fd");
            require(source.handle() == GHandle::invalid(), "moved-from handle is invalid");
            require(source.get_controller() == nullptr, "moved-from controller is empty");
            const auto size = source.size();
            const auto sync = source.sync();
            require(!size && size.error().code() == kClosed, "moved-from size reports closed");
            require(!sync && sync.error().code() == kClosed, "moved-from sync reports closed");

            const auto reopened = source.open("/dev/null", FileOpenMode::Read);
            require(reopened.has_value(), "moved-from file can be reopened");
            require(source.get_controller() != controller, "reopening uses an independent controller");
            destination = std::move(moved);
            require(destination.get_controller() == controller, "move assignment preserves controller address");
        }
        require(!is_closed(transferred_fd), "source destruction does not close transferred fd");
        auto& self = destination;
        auto& assigned = (destination = std::move(self));
        require(&assigned == &destination && destination.handle().fd == transferred_fd,
                "self-move preserves ownership");
    }
    require(is_closed(transferred_fd), "destination destruction closes transferred fd");

    auto source = make_file();
    auto target = make_file();
    const int replaced_fd = target.handle().fd;
    auto* controller = source.get_controller();
    target = std::move(source);
    require(is_closed(replaced_fd), "move assignment closes previous destination fd");
    require(target.get_controller() == controller, "replacement transfers original controller");
    const int adopted_fd = ::open("/dev/null", O_RDONLY);
    require(adopted_fd >= 0, "open fd for moved-from adoption");
    source.adopt(adopted_fd);
    require(source.handle().fd == adopted_fd && source.get_controller() != controller,
            "moved-from file can adopt an independent fd");
    AsyncFile owner(std::move(source));
    AsyncFile empty(std::move(source));
    const int target_fd = target.handle().fd;
    target = std::move(empty);
    require(is_closed(target_fd) && target.handle() == GHandle::invalid(),
            "assignment from moved-from file releases destination");
    require(owner.handle().fd == adopted_fd && !is_closed(adopted_fd),
            "empty moves leave the actual owner intact");
}

// 在 owner 线程注册读操作后立即移动源对象，覆盖 reactor 已借用控制器的情形。
struct MoveAfterReadSubmission {
    FileReadAwaitable* read;
    AsyncFile* source;
    AsyncFile* destination;

    bool await_ready() const noexcept { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        const bool suspended = read->await_suspend(handle);
        *destination = std::move(*source);
        return suspended;
    }
    std::expected<size_t, IOError> await_resume() { return read->await_resume(); }
};

Task<void> check_async_moves(AsyncFile* source, AsyncFile* destination, std::atomic<bool>* done) {
    std::array<char, kContent.size()> buffer{};
    auto* controller = source->get_controller();
    auto borrowed_read = source->read(buffer.data(), buffer.size());
    AsyncFile moved(std::move(*source));
    const auto read = co_await borrowed_read;
    require(read && *read == buffer.size(), "pre-move awaitable still completes");
    require(std::string_view(buffer.data(), buffer.size()) == kContent, "pre-move read contents");

    const int replaced_fd = destination->handle().fd;
    auto submitted_read = moved.read(buffer.data(), buffer.size());
    const auto pending_read = co_await MoveAfterReadSubmission{&submitted_read, &moved, destination};
    require(pending_read && *pending_read == buffer.size(), "read completes after moving registered controller");
    require(std::string_view(buffer.data(), buffer.size()) == kContent, "registered read contents");
    require(destination->get_controller() == controller, "registered controller address is unchanged");
    require(is_closed(replaced_fd), "registered move closes old destination fd");

    const auto closed_read = co_await source->read(buffer.data(), buffer.size());
    const auto closed_write = co_await source->write(kContent.data(), kContent.size());
    const auto closed_close = co_await source->close();
    require(!closed_read && closed_read.error().code() == kClosed, "moved-from read reports closed");
    require(!closed_write && closed_write.error().code() == kClosed, "moved-from write reports closed");
    require(!closed_close && closed_close.error().code() == kClosed, "moved-from close reports closed");

    const auto written = co_await destination->write("S", 1);
    require(written && *written == 1, "write after move");
    const auto read_back = co_await destination->read(buffer.data(), buffer.size());
    require(read_back && *read_back == buffer.size() &&
                std::string_view(buffer.data(), buffer.size()) == "Stable controller",
            "read back after move");
    const auto closed = co_await destination->close();
    require(closed.has_value() && destination->handle() == GHandle::invalid(),
            "logically close transferred file");
    done->store(true, std::memory_order_release);
}

} // namespace

int main() {
    check_ownership();
    auto source = make_file();
    auto destination = make_file();
    const int transferred_fd = source.handle().fd;
    std::atomic<bool> done{false};
    IOScheduler scheduler;
    const auto started = scheduler.start();
#ifdef USE_IOURING
    if (!started) {
        const auto error = static_cast<uint32_t>(started.error().code() >> 32);
        if (error == EPERM || error == ENOSYS || error == EOPNOTSUPP) {
            std::cout << "T193 SKIP: io_uring unavailable, errno=" << error << '\n';
            return 77;
        }
    }
#endif
    require(started.has_value(), "start IO scheduler");
    require(schedule_task(scheduler, check_async_moves(&source, &destination, &done)), "submit IO checks");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done.load(std::memory_order_acquire)) {
        require(std::chrono::steady_clock::now() < deadline, "IO checks complete before deadline");
        std::this_thread::sleep_for(1ms);
    }
    // io_uring 的逻辑 close 先返回，关闭 SQE 在后续 poll 提交；保持调度器运行至 fd 释放。
    while (!is_closed(transferred_fd)) {
        require(std::chrono::steady_clock::now() < deadline, "transferred fd is eventually closed");
        std::this_thread::sleep_for(1ms);
    }
    scheduler.stop();
    std::cout << "T193 AsyncFile move PASS\n";
    return 0;
}
#else
int main() { return 77; }
#endif
