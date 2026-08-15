#include "async_file.h"
#include "../coro-c/coro_wait.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static C_IOResult make_result_bytes(C_IOResultCode code, size_t bytes, int sys_errno)
{
    return (C_IOResult){code, sys_errno, bytes, 0, NULL};
}

static C_IOResult make_result_value(C_IOResultCode code, int64_t value, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, value, NULL};
}

static C_IOResult bind_current_scheduler(galay_c_async_file_t* file)
{
    galay_c_io_scheduler_t* const current = galay_c_io_scheduler_current();
    if (file == NULL || current == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    if (file->scheduler != NULL && file->scheduler != current) {
        return make_result(C_IOResultInvalid, 0);
    }
    file->scheduler = current;
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_async_file_open(galay_c_async_file_t* out_file,
                                   const char* path,
                                   uint32_t flags,
                                   uint32_t mode)
{
    if (!out_file || !path) {
        return make_result(C_IOResultInvalid, 0);
    }

    memset(out_file, 0, sizeof(*out_file));
    out_file->fd = -1;

    // 转换标志
    int open_flags = O_NONBLOCK;

    if ((flags & GALAY_C_FILE_RDWR) == GALAY_C_FILE_RDWR) {
        open_flags |= O_RDWR;
    } else if (flags & GALAY_C_FILE_WRITE) {
        open_flags |= O_WRONLY;
    } else {
        open_flags |= O_RDONLY;
    }

    if (flags & GALAY_C_FILE_CREATE) {
        open_flags |= O_CREAT;
    }
    if (flags & GALAY_C_FILE_TRUNC) {
        open_flags |= O_TRUNC;
    }
    if (flags & GALAY_C_FILE_APPEND) {
        open_flags |= O_APPEND;
    }

    // 打开文件
    int fd = open(path, open_flags, mode);
    if (fd < 0) {
        return make_result(C_IOResultError, errno);
    }

    out_file->fd = fd;
    out_file->position = 0;

    C_IOResult init_result = galay_c_io_controller_init(&out_file->controller, fd, out_file);
    if (init_result.code != C_IOResultOk) {
        const int close_result = close(fd);
        out_file->fd = -1;
        return close_result == 0 ? init_result : make_result(C_IOResultError, errno);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_async_file_read(galay_c_async_file_t* file,
                                          char* buffer,
                                          size_t length,
                                          int64_t timeout_ms)
{
    if (!file || file->fd < 0 || !buffer || length == 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(file);
    if (bound.code != C_IOResultOk) {
        return bound;
    }

    // 1. 尝试立即非阻塞读取
    ssize_t n = read(file->fd, buffer, length);
    if (n > 0) {
        file->position += n;
        return make_result_bytes(C_IOResultOk, (size_t)n, 0);
    }
    if (n == 0) {
        return make_result(C_IOResultEof, 0);
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }

    // 2. 挂起等待 READ 事件
    C_IOResult wait_result = galay_c_coro_wait_io(file->scheduler,
                                                   &file->controller,
                                                   GALAY_C_EVENT_READ,
                                                   timeout_ms);
    if (wait_result.code != C_IOResultOk) {
        return wait_result;
    }

    // 3. 恢复后重试读取
    n = read(file->fd, buffer, length);
    if (n > 0) {
        file->position += n;
        return make_result_bytes(C_IOResultOk, (size_t)n, 0);
    }
    if (n == 0) {
        return make_result(C_IOResultEof, 0);
    }

    return make_result(C_IOResultError, errno);
}

C_IOResult galay_c_async_file_write(galay_c_async_file_t* file,
                                           const char* buffer,
                                           size_t length,
                                           int64_t timeout_ms)
{
    if (!file || file->fd < 0 || !buffer || length == 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult bound = bind_current_scheduler(file);
    if (bound.code != C_IOResultOk) {
        return bound;
    }

    // 1. 尝试立即非阻塞写入
    ssize_t n = write(file->fd, buffer, length);
    if (n > 0) {
        file->position += n;
        return make_result_bytes(C_IOResultOk, (size_t)n, 0);
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return make_result(C_IOResultError, errno);
    }

    // 2. 挂起等待 WRITE 事件
    C_IOResult wait_result = galay_c_coro_wait_io(file->scheduler,
                                                   &file->controller,
                                                   GALAY_C_EVENT_WRITE,
                                                   timeout_ms);
    if (wait_result.code != C_IOResultOk) {
        return wait_result;
    }

    // 3. 恢复后重试写入
    n = write(file->fd, buffer, length);
    if (n > 0) {
        file->position += n;
        return make_result_bytes(C_IOResultOk, (size_t)n, 0);
    }

    return make_result(C_IOResultError, errno);
}

C_IOResult galay_c_async_file_seek(galay_c_async_file_t* file,
                                          int64_t offset,
                                          int whence)
{
    if (!file || file->fd < 0) {
        return make_result(C_IOResultInvalid, 0);
    }

    off_t new_pos = lseek(file->fd, offset, whence);
    if (new_pos < 0) {
        return make_result(C_IOResultError, errno);
    }

    file->position = new_pos;
    return make_result_value(C_IOResultOk, new_pos, 0);
}

C_IOResult galay_c_async_file_tell(galay_c_async_file_t* file)
{
    if (!file || file->fd < 0) {
        return make_result(C_IOResultInvalid, 0);
    }

    return make_result_value(C_IOResultOk, file->position, 0);
}

C_IOResult galay_c_async_file_close(galay_c_async_file_t* file)
{
    if (!file) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (file->fd < 0) {
        return make_result(C_IOResultOk, 0);
    }

    C_IOResult first_error = make_result(C_IOResultOk, 0);
    if (file->scheduler) {
        if (atomic_load_explicit(&file->controller.read_slot, memory_order_acquire) != NULL) {
            const C_IOResult cancelled = galay_c_coro_cancel_io(
                file->scheduler, &file->controller, GALAY_C_EVENT_READ);
            if (cancelled.code != C_IOResultCancelled && first_error.code == C_IOResultOk) {
                first_error = cancelled;
            }
        }
        if (atomic_load_explicit(&file->controller.write_slot, memory_order_acquire) != NULL) {
            const C_IOResult cancelled = galay_c_coro_cancel_io(
                file->scheduler, &file->controller, GALAY_C_EVENT_WRITE);
            if (cancelled.code != C_IOResultCancelled && first_error.code == C_IOResultOk) {
                first_error = cancelled;
            }
        }
        if (atomic_load_explicit(&file->controller.registered_events,
                                 memory_order_acquire) != GALAY_C_EVENT_NONE) {
            const C_IOResult unregistered =
                galay_c_io_scheduler_unregister(file->scheduler, &file->controller);
            if (unregistered.code != C_IOResultOk && first_error.code == C_IOResultOk) {
                first_error = unregistered;
            }
        }
    }
    const C_IOResult cleaned = galay_c_io_controller_cleanup(&file->controller);
    if (cleaned.code != C_IOResultOk && first_error.code == C_IOResultOk) {
        first_error = cleaned;
    }
    if (close(file->fd) != 0 && first_error.code == C_IOResultOk) {
        first_error = make_result(C_IOResultError, errno);
    }
    file->fd = -1;
    file->scheduler = NULL;
    return first_error;
}
