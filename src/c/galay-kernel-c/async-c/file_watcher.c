#include "file_watcher.h"
#include "../coro-c/coro_wait.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#define GALAY_HAS_INOTIFY 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#include <fcntl.h>
#define GALAY_HAS_KQUEUE 1
#endif

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static C_IOResult make_result_value(C_IOResultCode code, int64_t value, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, value, NULL};
}

static C_IOResult bind_current_scheduler(galay_c_file_watcher_t* watcher)
{
    galay_c_io_scheduler_t* const current = galay_c_io_scheduler_current();
    if (watcher == NULL || current == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (watcher->scheduler != NULL && watcher->scheduler != current) {
        return make_result(C_IOResultInvalid, 0);
    }
    watcher->scheduler = current;
    return make_result(C_IOResultOk, 0);
}

#ifdef GALAY_HAS_INOTIFY

C_IOResult galay_c_file_watcher_create(galay_c_file_watcher_t* out_watcher)
{
    if (!out_watcher) {
        return make_result(C_IOResultInvalid, 0);
    }
    memset(out_watcher, 0, sizeof(*out_watcher));
    out_watcher->fd = -1;

    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        return make_result(C_IOResultError, errno);
    }

    out_watcher->fd = fd;

    C_IOResult init_result = galay_c_io_controller_init(&out_watcher->controller, fd, out_watcher);
    if (init_result.code != C_IOResultOk) {
        const int close_result = close(fd);
        out_watcher->fd = -1;
        return close_result == 0 ? init_result : make_result(C_IOResultError, errno);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_file_watcher_add_watch(galay_c_file_watcher_t* watcher,
                                                 const char* path,
                                                 uint32_t events)
{
    if (!watcher || watcher->fd < 0 || !path || events == 0) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 转换事件掩码
    uint32_t mask = 0;
    if (events & GALAY_C_WATCH_CREATE) {
        mask |= IN_CREATE;
    }
    if (events & GALAY_C_WATCH_DELETE) {
        mask |= IN_DELETE | IN_DELETE_SELF;
    }
    if (events & GALAY_C_WATCH_MODIFY) {
        mask |= IN_MODIFY | IN_CLOSE_WRITE;
    }
    if (events & GALAY_C_WATCH_MOVE) {
        mask |= IN_MOVE | IN_MOVED_FROM | IN_MOVED_TO;
    }
    if (events & GALAY_C_WATCH_ATTRIB) {
        mask |= IN_ATTRIB;
    }

    int wd = inotify_add_watch(watcher->fd, path, mask);
    if (wd < 0) {
        return make_result(C_IOResultError, errno);
    }

    return make_result_value(C_IOResultOk, wd, 0);
}

C_IOResult galay_c_file_watcher_remove_watch(galay_c_file_watcher_t* watcher,
                                                    int wd)
{
    if (!watcher || watcher->fd < 0 || wd < 0) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (inotify_rm_watch(watcher->fd, wd) < 0) {
        return make_result(C_IOResultError, errno);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_file_watcher_wait(galay_c_file_watcher_t* watcher,
                                            galay_c_file_event_t* out_event,
                                            int64_t timeout_ms)
{
    if (!watcher || watcher->fd < 0 || !out_event || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(watcher);
    if (bound.code != C_IOResultOk) {
        return bound;
    }

    // 1. 尝试立即读取事件
    char buffer[sizeof(struct inotify_event) + NAME_MAX + 1];
    ssize_t n = read(watcher->fd, buffer, sizeof(buffer));

    if (n > 0) {
        // 解析事件
        struct inotify_event* event = (struct inotify_event*)buffer;

        memset(out_event, 0, sizeof(*out_event));
        out_event->mask = 0;
        out_event->cookie = event->cookie;
        out_event->is_dir = (event->mask & IN_ISDIR) ? 1 : 0;

        if (event->len > 0) {
            strncpy(out_event->name, event->name, sizeof(out_event->name) - 1);
        }

        // 转换事件掩码
        if (event->mask & IN_CREATE) {
            out_event->mask |= GALAY_C_WATCH_CREATE;
        }
        if (event->mask & (IN_DELETE | IN_DELETE_SELF)) {
            out_event->mask |= GALAY_C_WATCH_DELETE;
        }
        if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
            out_event->mask |= GALAY_C_WATCH_MODIFY;
        }
        if (event->mask & (IN_MOVE | IN_MOVED_FROM | IN_MOVED_TO)) {
            out_event->mask |= GALAY_C_WATCH_MOVE;
        }
        if (event->mask & IN_ATTRIB) {
            out_event->mask |= GALAY_C_WATCH_ATTRIB;
        }

        return make_result(C_IOResultOk, 0);
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }

    // 2. 挂起等待 READ 事件
    C_IOResult wait_result = galay_c_coro_wait_io(watcher->scheduler,
                                                   &watcher->controller,
                                                   GALAY_C_EVENT_READ,
                                                   timeout_ms);
    if (wait_result.code != C_IOResultOk) {
        return wait_result;
    }

    // 3. 恢复后重试读取
    n = read(watcher->fd, buffer, sizeof(buffer));
    if (n <= 0) {
        return make_result(C_IOResultError, errno);
    }

    // 解析事件
    struct inotify_event* event = (struct inotify_event*)buffer;

    memset(out_event, 0, sizeof(*out_event));
    out_event->mask = 0;
    out_event->cookie = event->cookie;
    out_event->is_dir = (event->mask & IN_ISDIR) ? 1 : 0;

    if (event->len > 0) {
        strncpy(out_event->name, event->name, sizeof(out_event->name) - 1);
    }

    if (event->mask & IN_CREATE) {
        out_event->mask |= GALAY_C_WATCH_CREATE;
    }
    if (event->mask & (IN_DELETE | IN_DELETE_SELF)) {
        out_event->mask |= GALAY_C_WATCH_DELETE;
    }
    if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
        out_event->mask |= GALAY_C_WATCH_MODIFY;
    }
    if (event->mask & (IN_MOVE | IN_MOVED_FROM | IN_MOVED_TO)) {
        out_event->mask |= GALAY_C_WATCH_MOVE;
    }
    if (event->mask & IN_ATTRIB) {
        out_event->mask |= GALAY_C_WATCH_ATTRIB;
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_file_watcher_close(galay_c_file_watcher_t* watcher)
{
    if (!watcher) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (watcher->fd < 0) {
        return make_result(C_IOResultOk, 0);
    }

    C_IOResult first_error = make_result(C_IOResultOk, 0);
    if (watcher->scheduler) {
        if (atomic_load_explicit(&watcher->controller.read_slot,
                                 memory_order_acquire) != NULL) {
            const C_IOResult cancelled = galay_c_coro_cancel_io(
                watcher->scheduler, &watcher->controller, GALAY_C_EVENT_READ);
            if (cancelled.code != C_IOResultCancelled) {
                first_error = cancelled;
            }
        }
        if (atomic_load_explicit(&watcher->controller.registered_events,
                                 memory_order_acquire) != GALAY_C_EVENT_NONE) {
            const C_IOResult unregistered =
                galay_c_io_scheduler_unregister(watcher->scheduler,
                                                &watcher->controller);
            if (unregistered.code != C_IOResultOk && first_error.code == C_IOResultOk) {
                first_error = unregistered;
            }
        }
    }

    const C_IOResult cleaned = galay_c_io_controller_cleanup(&watcher->controller);
    if (cleaned.code != C_IOResultOk && first_error.code == C_IOResultOk) {
        first_error = cleaned;
    }

    if (close(watcher->fd) != 0 && first_error.code == C_IOResultOk) {
        first_error = make_result(C_IOResultError, errno);
    }
    watcher->fd = -1;
    watcher->scheduler = NULL;

    return first_error;
}

#elif defined(GALAY_HAS_KQUEUE)

// kqueue 实现（macOS/BSD）
C_IOResult galay_c_file_watcher_create(galay_c_file_watcher_t* out_watcher)
{
    if (!out_watcher) {
        return make_result(C_IOResultInvalid, 0);
    }
    memset(out_watcher, 0, sizeof(*out_watcher));
    out_watcher->fd = -1;

    int kq = kqueue();
    if (kq < 0) {
        return make_result(C_IOResultError, errno);
    }

    out_watcher->fd = kq;

    C_IOResult init_result = galay_c_io_controller_init(&out_watcher->controller, kq, out_watcher);
    if (init_result.code != C_IOResultOk) {
        const int close_result = close(kq);
        out_watcher->fd = -1;
        return close_result == 0 ? init_result : make_result(C_IOResultError, errno);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_file_watcher_add_watch(galay_c_file_watcher_t* watcher,
                                                 const char* path,
                                                 uint32_t events)
{
    if (!watcher || !path) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 打开文件获取 fd
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return make_result(C_IOResultError, errno);
    }

    // 设置 kqueue 事件
    struct kevent kev;
    uint32_t fflags = 0;

    if (events & GALAY_C_WATCH_CREATE) {
        fflags |= NOTE_WRITE;
    }
    if (events & GALAY_C_WATCH_DELETE) {
        fflags |= NOTE_DELETE;
    }
    if (events & GALAY_C_WATCH_MODIFY) {
        fflags |= NOTE_WRITE | NOTE_EXTEND;
    }
    if (events & GALAY_C_WATCH_MOVE) {
        fflags |= NOTE_RENAME;
    }
    if (events & GALAY_C_WATCH_ATTRIB) {
        fflags |= NOTE_ATTRIB;
    }

    EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, fflags, 0, NULL);

    if (kevent(watcher->fd, &kev, 1, NULL, 0, NULL) < 0) {
        const int saved_errno = errno;
        const int close_result = close(fd);
        return make_result(C_IOResultError,
                           close_result == 0 ? saved_errno : errno);
    }

    // 返回 fd 作为 watch descriptor
    return make_result_value(C_IOResultOk, fd, 0);
}

C_IOResult galay_c_file_watcher_remove_watch(galay_c_file_watcher_t* watcher,
                                                    int wd)
{
    if (!watcher || wd < 0) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 移除 kqueue 事件并关闭 fd
    struct kevent kev;
    EV_SET(&kev, wd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    C_IOResult result = kevent(watcher->fd, &kev, 1, NULL, 0, NULL) == 0
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultError, errno);
    if (close(wd) != 0 && result.code == C_IOResultOk) {
        result = make_result(C_IOResultError, errno);
    }
    return result;
}

C_IOResult galay_c_file_watcher_wait(galay_c_file_watcher_t* watcher,
                                            galay_c_file_event_t* out_event,
                                            int64_t timeout_ms)
{
    if (!watcher || watcher->fd < 0 || !out_event || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(watcher);
    if (bound.code != C_IOResultOk) {
        return bound;
    }

    struct kevent kev;
    struct timespec ts;
    struct timespec* ts_ptr = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }

    // kevent 等待
    int n = kevent(watcher->fd, NULL, 0, &kev, 1, ts_ptr);
    if (n < 0) {
        return make_result(C_IOResultError, errno);
    }
    if (n == 0) {
        return make_result(C_IOResultTimeout, 0);
    }

    // 解析事件
    memset(out_event, 0, sizeof(*out_event));
    out_event->mask = 0;

    if (kev.fflags & NOTE_DELETE) {
        out_event->mask |= GALAY_C_WATCH_DELETE;
    }
    if (kev.fflags & (NOTE_WRITE | NOTE_EXTEND)) {
        out_event->mask |= GALAY_C_WATCH_MODIFY;
    }
    if (kev.fflags & NOTE_RENAME) {
        out_event->mask |= GALAY_C_WATCH_MOVE;
    }
    if (kev.fflags & NOTE_ATTRIB) {
        out_event->mask |= GALAY_C_WATCH_ATTRIB;
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_file_watcher_close(galay_c_file_watcher_t* watcher)
{
    if (!watcher) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (watcher->fd < 0) {
        return make_result(C_IOResultOk, 0);
    }

    C_IOResult first_error = make_result(C_IOResultOk, 0);
    if (watcher->scheduler) {
        if (atomic_load_explicit(&watcher->controller.read_slot,
                                 memory_order_acquire) != NULL) {
            const C_IOResult cancelled = galay_c_coro_cancel_io(
                watcher->scheduler, &watcher->controller, GALAY_C_EVENT_READ);
            if (cancelled.code != C_IOResultCancelled) {
                first_error = cancelled;
            }
        }
        if (atomic_load_explicit(&watcher->controller.registered_events,
                                 memory_order_acquire) != GALAY_C_EVENT_NONE) {
            const C_IOResult unregistered =
                galay_c_io_scheduler_unregister(watcher->scheduler,
                                                &watcher->controller);
            if (unregistered.code != C_IOResultOk && first_error.code == C_IOResultOk) {
                first_error = unregistered;
            }
        }
    }

    const C_IOResult cleaned = galay_c_io_controller_cleanup(&watcher->controller);
    if (cleaned.code != C_IOResultOk && first_error.code == C_IOResultOk) {
        first_error = cleaned;
    }

    if (close(watcher->fd) != 0 && first_error.code == C_IOResultOk) {
        first_error = make_result(C_IOResultError, errno);
    }
    watcher->fd = -1;
    watcher->scheduler = NULL;

    return first_error;
}

#else

// 平台不支持
C_IOResult galay_c_file_watcher_create(galay_c_file_watcher_t* out_watcher,
                                              galay_c_io_scheduler_t* scheduler)
{
    return make_result(C_IOResultError, ENOSYS);
}

C_IOResult galay_c_file_watcher_add_watch(galay_c_file_watcher_t* watcher,
                                                 const char* path,
                                                 uint32_t events)
{
    return make_result(C_IOResultError, ENOSYS);
}

C_IOResult galay_c_file_watcher_remove_watch(galay_c_file_watcher_t* watcher,
                                                    int wd)
{
    return make_result(C_IOResultError, ENOSYS);
}

C_IOResult galay_c_file_watcher_wait(galay_c_file_watcher_t* watcher,
                                            galay_c_file_event_t* out_event,
                                            int64_t timeout_ms)
{
    return make_result(C_IOResultError, ENOSYS);
}

C_IOResult galay_c_file_watcher_close(galay_c_file_watcher_t* watcher)
{
    return make_result(C_IOResultError, ENOSYS);
}

#endif
