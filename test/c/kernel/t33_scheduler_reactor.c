#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    galay_c_io_scheduler_t scheduler = {0};
    assert(galay_c_io_scheduler_create(NULL, NULL).code == C_IOResultInvalid);
    assert(galay_c_io_scheduler_create(&scheduler, NULL).code == C_IOResultOk);
    const int descriptor_flags = fcntl(scheduler.reactor_fd, F_GETFD);
    assert(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const int flags = fcntl(sockets[0], F_GETFL);
    assert(flags >= 0 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
    galay_c_io_controller_t controller = {0};
    assert(galay_c_io_controller_init(&controller, sockets[0], NULL).code == C_IOResultOk);

    const uint32_t masks[] = {
        GALAY_C_EVENT_READ,
        GALAY_C_EVENT_READ | GALAY_C_EVENT_WRITE,
        GALAY_C_EVENT_WRITE,
        GALAY_C_EVENT_READ,
    };
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        assert(galay_c_io_scheduler_register(&scheduler, &controller, masks[i]).code ==
               C_IOResultOk);
        assert(atomic_load(&controller.registered_events) == masks[i]);
        assert(galay_c_io_scheduler_modify(&scheduler, &controller, masks[i]).code ==
               C_IOResultOk);
    }

    assert(galay_c_io_scheduler_unregister(&scheduler, &controller).code == C_IOResultOk);
    assert(atomic_load(&controller.registered_events) == GALAY_C_EVENT_NONE);
    controller.fd = -1;
    const C_IOResult failed =
        galay_c_io_scheduler_register(&scheduler, &controller, GALAY_C_EVENT_READ);
    assert(failed.code == C_IOResultError && failed.sys_errno == EBADF);
    assert(atomic_load(&controller.registered_events) == GALAY_C_EVENT_NONE);

    assert(galay_c_io_controller_cleanup(&controller).code == C_IOResultOk);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
    const int reactor_fd = scheduler.reactor_fd;
    assert(galay_c_io_scheduler_destroy(&scheduler).code == C_IOResultOk);
    assert(fcntl(reactor_fd, F_GETFD) == -1 && errno == EBADF);
    return 0;
}
