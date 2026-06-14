/**
 * @file module_prelude.hpp
 * @brief C++23 模块构建的自动前导头文件
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 为 Clang/GCC/MSVC 的过渡性 C++23 模块构建提供自动前导。
 *          将第三方、系统和依赖头文件放入全局模块片段中，
 *          确保模块构建时正确隔离标准库和项目依赖。
 */

#pragma once

#if __has_include(<array>)
#include <array>
#endif
#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<chrono>)
#include <chrono>
#endif
#if __has_include(<concepts>)
#include <concepts>
#endif
#if __has_include(<condition_variable>)
#include <condition_variable>
#endif
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if __has_include(<cstddef>)
#include <cstddef>
#endif
#if __has_include(<cstdint>)
#include <cstdint>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<kernel/async/tcp_socket.h>)
#include <kernel/async/tcp_socket.h>
#endif
#if __has_include(<utils/cache/ring_buffer.hpp>)
#include <utils/cache/ring_buffer.hpp>
#endif
#if __has_include(<kernel/common/error.h>)
#include <kernel/common/error.h>
#endif
#if __has_include(<kernel/common/host.hpp>)
#include <kernel/common/host.hpp>
#endif
#if __has_include(<kernel/concurrency/async_waiter.h>)
#include <kernel/concurrency/async_waiter.h>
#endif
#if __has_include(<kernel/kernel/awaitable.h>)
#include <kernel/kernel/awaitable.h>
#endif
#if __has_include(<kernel/kernel/io_scheduler.hpp>)
#include <kernel/kernel/io_scheduler.hpp>
#endif
#if __has_include(<kernel/kernel/task.h>)
#include <kernel/kernel/task.h>
#endif
#if __has_include(<kernel/kernel/timeout.hpp>)
#include <kernel/kernel/timeout.hpp>
#endif
#if __has_include(<map>)
#include <map>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<mutex>)
#include <mutex>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<queue>)
#include <queue>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<type_traits>)
#include <type_traits>
#endif
#if __has_include(<unordered_map>)
#include <unordered_map>
#endif
#if __has_include(<utility>)
#include <utility>
#endif
#if __has_include(<variant>)
#include <variant>
#endif
#if __has_include(<vector>)
#include <vector>
#endif
#if __has_include("redis/base/redis_config.h")
#include "redis/base/redis_config.h"
#endif
#if __has_include(<ssl/async/ssl_await.h>)
#include <ssl/async/ssl_await.h>
#endif
#if __has_include(<ssl/async/ssl_socket.h>)
#include <ssl/async/ssl_socket.h>
#endif
#if __has_include(<ssl/ssl/ssl_context.h>)
#include <ssl/ssl/ssl_context.h>
#endif
