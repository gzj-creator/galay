/**
 * @file t195_epoll_pending_registration.cc
 * @brief 确定性复现 epoll 注销/销毁 controller 后的 pending 索引残留。
 * 不启动 scheduler，不依赖时序；后续注册必须保留每个 fd 的读兴趣。
 */
#include <galay/cpp/galay-kernel/core/epoll_reactor.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <optional>

#ifdef USE_EPOLL
#include <sys/socket.h>
#include <unistd.h>

using namespace galay::kernel;

namespace galay::kernel {
struct EpollReactorTestAccess {
    static size_t pending_count(const EpollReactor& reactor) {
        return reactor.m_pending_changes.size();
    }
    static size_t pending_index_count(const EpollReactor& reactor) {
        return reactor.m_pending_change_index.size();
    }
};
}

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        // 已判定失败，诊断输出不改变退出码。
        (void)std::fprintf(stderr, "epoll pending registration: %s\n", message);
        std::exit(1);
    }
}

struct SocketPair {
    int fds[2];

    SocketPair()
    {
        require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0,
                "create socket pair");
    }
    ~SocketPair()
    {
        require(::close(fds[0]) == 0, "close local fd");
        require(::close(fds[1]) == 0, "close peer fd");
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
};

void check_read_registration(EpollReactor& reactor, IOController& controller, const SocketPair& pair)
{
    require(controller.m_registered_events == (EPOLLIN | EPOLLET),
            "each controller retains its own read registration");
    require(::send(pair.fds[1], "x", 1, 0) == 1, "send readiness probe");
    epoll_event event{};
    require(::epoll_wait(reactor.get_poll_handle().fd, &event, 1, 0) == 1 &&
                (event.events & EPOLLIN) != 0,
            "new read interest reaches epoll");
    char byte = 0;
    require(::recv(pair.fds[0], &byte, 1, 0) == 1 && byte == 'x',
            "read readiness probe");
}

void retired_registration_can_be_queued_again(EpollReactor& reactor)
{
    SocketPair first_pair;
    SocketPair second_pair;
    IOController first(GHandle{.fd = first_pair.fds[0]});
    IOController second(GHandle{.fd = second_pair.fds[0]});

    require(reactor.add_file_write(&first) == 0 && reactor.flush_pending_changes() == 0,
            "register initial write interest");
    require(reactor.remove(&first) == 0 && reactor.flush_pending_changes() == 0,
            "retire initial registration");
    require(first.m_registration_owner_slot == nullptr && first.m_registered_events == 0,
            "initial registration is retired");

    // Reuse the old queue index for another fd before rearming the first fd.
    require(reactor.add_file_read(&second) == 0, "queue second fd");
    require(reactor.add_file_read(&first) == 0, "requeue first fd");
    require(reactor.flush_pending_changes() == 0, "flush both read interests");
    check_read_registration(reactor, first, first_pair);
    check_read_registration(reactor, second, second_pair);
    require(reactor.remove(&first) == 0 && reactor.remove(&second) == 0 &&
                reactor.flush_pending_changes() == 0, "remove read interests");
}

void destroyed_pending_controller_leaves_no_index(EpollReactor& reactor)
{
    SocketPair first_pair;
    SocketPair second_pair;
    std::optional<IOController> first;
    auto* const first_address = &first.emplace(GHandle{.fd = first_pair.fds[0]});
    IOController second(GHandle{.fd = second_pair.fds[0]});
    require(reactor.add_file_read(&*first) == 0, "queue controller before destruction");
    first.reset();
    require(reactor.flush_pending_changes() == 0, "discard destroyed controller's change");

    // optional reuses the exact controller address, without allocator timing.
    auto& recreated = first.emplace(GHandle{.fd = first_pair.fds[0]});
    require(&recreated == first_address, "reuse controller storage");
    require(reactor.add_file_read(&second) == 0 && reactor.add_file_read(&*first) == 0 &&
                reactor.flush_pending_changes() == 0, "register after controller address reuse");
    check_read_registration(reactor, *first, first_pair);
    check_read_registration(reactor, second, second_pair);
    require(reactor.remove(&*first) == 0 && reactor.remove(&second) == 0 &&
                reactor.flush_pending_changes() == 0, "remove recreated read interests");
}

void cancelled_middle_change_keeps_swapped_index(EpollReactor& reactor)
{
    SocketPair pairs[3];
    IOController first(GHandle{.fd = pairs[0].fds[0]});
    IOController middle(GHandle{.fd = pairs[1].fds[0]});
    IOController last(GHandle{.fd = pairs[2].fds[0]});
    require(reactor.add_file_read(&first) == 0 && reactor.add_file_read(&middle) == 0 &&
                reactor.add_file_read(&last) == 0, "queue three changes");
    require(reactor.remove(&middle) == 0 && reactor.remove(&last) == 0,
            "cancel middle and swapped changes");
    require(reactor.flush_pending_changes() == 0, "flush surviving change");
    require(middle.m_registered_events == 0 && last.m_registered_events == 0,
            "cancelled changes stay unregistered");
    check_read_registration(reactor, first, pairs[0]);
    require(reactor.remove(&first) == 0 && reactor.flush_pending_changes() == 0,
            "remove surviving read interest");
}

void existing_entry_survives_merged_and_cancelled_changes(EpollReactor& reactor)
{
    SocketPair pair;
    IOController controller(GHandle{.fd = pair.fds[0]});
    require(reactor.add_file_write(&controller) == 0 &&
                reactor.flush_pending_changes() == 0, "register reusable entry");
    auto* const owner_slot = controller.m_registration_owner_slot;
    require(owner_slot != nullptr && *owner_slot == &controller,
            "initial registration owns its stable slot");

    for (int round = 0; round < 64; ++round) {
        require(reactor.add_file_read(&controller) == 0 &&
                    reactor.add_file_read(&controller) == 0,
                "merge repeated read registration");
        require(EpollReactorTestAccess::pending_count(reactor) == 1 &&
                    EpollReactorTestAccess::pending_index_count(reactor) == 1,
                "repeated event has exactly one pending index");
        require(reactor.add_file_write(&controller) == 0,
                "return to registered events cancels pending change");
        require(EpollReactorTestAccess::pending_count(reactor) == 0 &&
                    EpollReactorTestAccess::pending_index_count(reactor) == 0,
                "cancelled change removes both pending records");
        require(controller.m_registration_owner_slot == owner_slot &&
                    *owner_slot == &controller,
                "cancellation retains registration ownership");
    }

    require(reactor.add_file_read(&controller) == 0 &&
                reactor.flush_pending_changes() == 0, "flush reused registration entry");
    require(controller.m_registration_owner_slot == owner_slot &&
                *owner_slot == &controller,
            "requeued registration uses the same owner slot");
    check_read_registration(reactor, controller, pair);
    require(reactor.remove(&controller) == 0 && reactor.flush_pending_changes() == 0,
            "remove reused registration");
    require(controller.m_registration_owner_slot == nullptr &&
                EpollReactorTestAccess::pending_count(reactor) == 0 &&
                EpollReactorTestAccess::pending_index_count(reactor) == 0,
            "retired registration leaves no pending entry");
}

} // namespace
#endif

int main()
{
#ifdef USE_EPOLL
    std::atomic<uint64_t> last_error{0};
    EpollReactor reactor(16, last_error);
    require(reactor.start().has_value(), "start reactor");
    retired_registration_can_be_queued_again(reactor);
    destroyed_pending_controller_leaves_no_index(reactor);
    cancelled_middle_change_keeps_swapped_index(reactor);
    existing_entry_survives_merged_and_cancelled_changes(reactor);
    require(last_error.load() == 0, "reactor reports no backend errors");
    require(std::puts("epoll pending registration PASS") >= 0, "print test result");
#else
    if (std::puts("epoll pending registration SKIP (requires epoll)") < 0) {
        return 1;
    }
#endif
    return 0;
}
