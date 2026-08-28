/**
 * @file module_prelude.hpp
 * @brief PostgreSQL C++23 module global-fragment dependencies.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/common/host.hpp>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/timeout.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>
#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

#include <galay/thirdparty/concurrentqueue/moodycamel/concurrentqueue.h>
