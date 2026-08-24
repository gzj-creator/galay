#include "aio_file.h"
#include "../common-c/macro.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(GALAY_HAS_IOURING)
#include <liburing.h>
#endif

#if defined(GALAY_HAS_AIO)
#include <aio.h>
#endif

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

C_IOResult galay_c_aio_check_support(galay_c_aio_backend_t* out_backend)
{
    if (!out_backend) {
        return make_result(C_IOResultInvalid, 0);
    }

#ifdef GALAY_HAS_IOURING
    // 检查 io_uring 是否可用
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) == 0) {
        io_uring_queue_exit(&ring);
        *out_backend = GALAY_C_AIO_BACKEND_IOURING;
        return make_result(C_IOResultOk, 0);
    }
#endif

#ifdef GALAY_HAS_AIO
    // 回退到 POSIX aio
    *out_backend = GALAY_C_AIO_BACKEND_AIO;
    return make_result(C_IOResultOk, 0);
#endif

    // 最后回退到线程池模拟（未实现）
    *out_backend = GALAY_C_AIO_BACKEND_FALLBACK;
    return make_result(C_IOResultError, ENOSYS);
}

// io_uring 后端实现
#ifdef GALAY_HAS_IOURING

typedef struct iouring_context {
    struct io_uring ring;
    int initialized;
} iouring_context_t;

static C_IOResult iouring_init(galay_c_aio_file_t* file)
{
    iouring_context_t* ctx = calloc(1, sizeof(iouring_context_t));
    if (!ctx) {
        return make_result(C_IOResultError, ENOMEM);
    }

    if (io_uring_queue_init(32, &ctx->ring, 0) < 0) {
        free(ctx);
        return make_result(C_IOResultError, errno);
    }

    ctx->initialized = 1;
    file->backend_context = ctx;
    return make_result(C_IOResultOk, 0);
}

static void iouring_cleanup(galay_c_aio_file_t* file)
{
    if (!file->backend_context) {
        return;
    }

    iouring_context_t* ctx = file->backend_context;
    if (ctx->initialized) {
        io_uring_queue_exit(&ctx->ring);
    }
    free(ctx);
    file->backend_context = NULL;
}

static C_IOResult iouring_read(galay_c_aio_file_t* file,
                               char* buffer,
                               size_t length,
                               int64_t offset,
                               int64_t timeout_ms)
{
    iouring_context_t* ctx = file->backend_context;
    if (!ctx || !ctx->initialized) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 获取 SQE
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        return make_result(C_IOResultError, ENOMEM);
    }

    // 准备读操作
    off_t read_offset = (offset >= 0) ? offset : file->position;
    io_uring_prep_read(sqe, file->fd, buffer, length, read_offset);

    // 提交
    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        return make_result(C_IOResultError, -ret);
    }

    // 等待完成
    struct io_uring_cqe* cqe;
    struct __kernel_timespec ts;
    struct __kernel_timespec* ts_ptr = NULL;

    if (timeout_ms > 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }

    ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, ts_ptr);
    if (ret < 0) {
        if (ret == -ETIME) {
            return make_result(C_IOResultTimeout, 0);
        }
        return make_result(C_IOResultError, -ret);
    }

    int result = cqe->res;
    io_uring_cqe_seen(&ctx->ring, cqe);

    if (result < 0) {
        return make_result(C_IOResultError, -result);
    }
    if (result == 0) {
        return make_result(C_IOResultEof, 0);
    }

    if (offset < 0) {
        file->position += result;
    }

    return make_result_bytes(C_IOResultOk, (size_t)result, 0);
}

static C_IOResult iouring_write(galay_c_aio_file_t* file,
                                const char* buffer,
                                size_t length,
                                int64_t offset,
                                int64_t timeout_ms)
{
    iouring_context_t* ctx = file->backend_context;
    if (!ctx || !ctx->initialized) {
        return make_result(C_IOResultInvalid, 0);
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        return make_result(C_IOResultError, ENOMEM);
    }

    off_t write_offset = (offset >= 0) ? offset : file->position;
    io_uring_prep_write(sqe, file->fd, buffer, length, write_offset);

    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        return make_result(C_IOResultError, -ret);
    }

    struct io_uring_cqe* cqe;
    struct __kernel_timespec ts;
    struct __kernel_timespec* ts_ptr = NULL;

    if (timeout_ms > 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }

    ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, ts_ptr);
    if (ret < 0) {
        if (ret == -ETIME) {
            return make_result(C_IOResultTimeout, 0);
        }
        return make_result(C_IOResultError, -ret);
    }

    int result = cqe->res;
    io_uring_cqe_seen(&ctx->ring, cqe);

    if (result < 0) {
        return make_result(C_IOResultError, -result);
    }

    if (offset < 0) {
        file->position += result;
    }

    return make_result_bytes(C_IOResultOk, (size_t)result, 0);
}

static C_IOResult iouring_fsync(galay_c_aio_file_t* file, int64_t timeout_ms)
{
    iouring_context_t* ctx = file->backend_context;
    if (!ctx || !ctx->initialized) {
        return make_result(C_IOResultInvalid, 0);
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        return make_result(C_IOResultError, ENOMEM);
    }

    io_uring_prep_fsync(sqe, file->fd, 0);

    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        return make_result(C_IOResultError, -ret);
    }

    struct io_uring_cqe* cqe;
    ret = io_uring_wait_cqe(&ctx->ring, &cqe);
    if (ret < 0) {
        return make_result(C_IOResultError, -ret);
    }

    int result = cqe->res;
    io_uring_cqe_seen(&ctx->ring, cqe);

    if (result < 0) {
        return make_result(C_IOResultError, -result);
    }

    return make_result(C_IOResultOk, 0);
}

#endif // GALAY_HAS_IOURING

// 公共 API 实现

C_IOResult galay_c_aio_file_open(galay_c_aio_file_t* out_file,
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
    int open_flags = 0;

    if ((flags & GALAY_C_AIO_RDWR) == GALAY_C_AIO_RDWR) {
        open_flags |= O_RDWR;
    } else if (flags & GALAY_C_AIO_WRITE) {
        open_flags |= O_WRONLY;
    } else {
        open_flags |= O_RDONLY;
    }

    if (flags & GALAY_C_AIO_CREATE) {
        open_flags |= O_CREAT;
    }
    if (flags & GALAY_C_AIO_TRUNC) {
        open_flags |= O_TRUNC;
    }
    if (flags & GALAY_C_AIO_DIRECT) {
#ifdef O_DIRECT
        open_flags |= O_DIRECT;
#endif
    }

    // 打开文件
    int fd = open(path, open_flags, mode);
    if (fd < 0) {
        return make_result(C_IOResultError, errno);
    }

    out_file->fd = fd;
    out_file->position = 0;

    // 检测并初始化后端
    galay_c_aio_backend_t backend;
    C_IOResult check_result = galay_c_aio_check_support(&backend);
    if (check_result.code != C_IOResultOk) {
        const int close_result = close(fd);
        out_file->fd = -1;
        return close_result == 0 ? check_result : make_result(C_IOResultError, errno);
    }

    out_file->backend = backend;

#ifdef GALAY_HAS_IOURING
    if (backend == GALAY_C_AIO_BACKEND_IOURING) {
        C_IOResult init_result = iouring_init(out_file);
        if (init_result.code != C_IOResultOk) {
            const int close_result = close(fd);
            out_file->fd = -1;
            return close_result == 0 ? init_result : make_result(C_IOResultError, errno);
        }
    }
#endif

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_aio_file_read(galay_c_aio_file_t* file,
                                        char* buffer,
                                        size_t length,
                                        int64_t offset,
                                        int64_t timeout_ms)
{
    if (!file || file->fd < 0 || !buffer || length == 0 || offset < -1 ||
        timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }

#ifdef GALAY_HAS_IOURING
    if (file->backend == GALAY_C_AIO_BACKEND_IOURING) {
        return iouring_read(file, buffer, length, offset, timeout_ms);
    }
#endif

    // 回退到同步 I/O（未实现 aio 后端）
    return make_result(C_IOResultError, ENOSYS);
}

C_IOResult galay_c_aio_file_write(galay_c_aio_file_t* file,
                                         const char* buffer,
                                         size_t length,
                                         int64_t offset,
                                         int64_t timeout_ms)
{
    if (!file || file->fd < 0 || !buffer || length == 0 || offset < -1 ||
        timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }

#ifdef GALAY_HAS_IOURING
    if (file->backend == GALAY_C_AIO_BACKEND_IOURING) {
        return iouring_write(file, buffer, length, offset, timeout_ms);
    }
#endif

    return make_result(C_IOResultError, ENOSYS);
}

C_IOResult galay_c_aio_file_fsync(galay_c_aio_file_t* file,
                                         int64_t timeout_ms)
{
    if (!file || file->fd < 0 || timeout_ms < -1) {
        return make_result(C_IOResultInvalid, 0);
    }

#ifdef GALAY_HAS_IOURING
    if (file->backend == GALAY_C_AIO_BACKEND_IOURING) {
        return iouring_fsync(file, timeout_ms);
    }
#endif

    // 回退到同步 fsync
    if (fsync(file->fd) < 0) {
        return make_result(C_IOResultError, errno);
    }
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_aio_file_seek(galay_c_aio_file_t* file,
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

C_IOResult galay_c_aio_file_close(galay_c_aio_file_t* file)
{
    if (!file) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (file->fd < 0) {
        return make_result(C_IOResultOk, 0);
    }

#ifdef GALAY_HAS_IOURING
    if (file->backend == GALAY_C_AIO_BACKEND_IOURING) {
        iouring_cleanup(file);
    }
#endif

    if (close(file->fd) != 0) {
        const int saved_errno = errno;
        file->fd = -1;
        return make_result(C_IOResultError, saved_errno);
    }
    file->fd = -1;

    return make_result(C_IOResultOk, 0);
}
