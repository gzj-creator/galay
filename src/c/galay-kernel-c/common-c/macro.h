/**
 * @file macro.h
 * @brief galay-kernel C ABI 编译期配置宏
 */

#ifndef GALAY_C_KERNEL_COMMON_MACRO_H
#define GALAY_C_KERNEL_COMMON_MACRO_H

#include <stddef.h>

#ifndef C_HOST_ADDRESS_MAX_LENGTH
#define C_HOST_ADDRESS_MAX_LENGTH 46
#endif

#ifndef C_RUNTIME_SCHEDULER_COUNT_AUTO
#define C_RUNTIME_SCHEDULER_COUNT_AUTO ((size_t)-1)
#endif

#if defined(__linux__)
#define GALAY_HAS_INOTIFY 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define GALAY_HAS_KQUEUE 1
#endif

#if defined(__linux__) && !defined(GALAY_DISABLE_IOURING) && \
    __has_include(<linux/version.h>) && __has_include(<linux/io_uring.h>)
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#define GALAY_HAS_IOURING 1
#endif
#endif

#if defined(_POSIX_ASYNCHRONOUS_IO)
#define GALAY_HAS_AIO 1
#endif

#if (defined(__APPLE__) && defined(__aarch64__)) || \
    (defined(__linux__) && defined(__x86_64__))
#define GALAY_C_CORO_HAS_CONTEXT 1
#else
#define GALAY_C_CORO_HAS_CONTEXT 0
#endif

#endif  /* GALAY_C_KERNEL_COMMON_MACRO_H */
