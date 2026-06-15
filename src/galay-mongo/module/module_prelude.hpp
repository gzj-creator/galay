/**
 * @file module_prelude.hpp
 * @brief galay-mongo 模块统一头文件前置包含
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 使用 __has_include 条件编译，按需包含 galay-mongo 所需的所有标准库和
 * 第三方头文件。作为单点导入入口，避免各模块重复列出依赖。
 */

#pragma once

#if __has_include(<algorithm>)
#include <algorithm>
#endif
#if __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif
#if __has_include(<charconv>)
#include <charconv>
#endif
#if __has_include(<chrono>)
#include <chrono>
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
#if __has_include(<cstring>)
#include <cstring>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#if __has_include(<galay-kernel/async/tcp_socket.h>)
#include <galay-kernel/async/tcp_socket.h>
#endif
#if __has_include(<galay-utils/cache/ring_buffer.hpp>)
#include <galay-utils/cache/ring_buffer.hpp>
#endif
#if __has_include(<galay-kernel/common/error.h>)
#include <galay-kernel/common/error.h>
#endif
#if __has_include(<galay-kernel/common/host.hpp>)
#include <galay-kernel/common/host.hpp>
#endif
#if __has_include(<galay-kernel/core/task.h>)
#include <galay-kernel/core/task.h>
#endif
#if __has_include(<galay-kernel/core/io_scheduler.hpp>)
#include <galay-kernel/core/io_scheduler.hpp>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<netdb.h>)
#include <netdb.h>
#endif
#if __has_include(<netinet/in.h>)
#include <netinet/in.h>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<poll.h>)
#include <poll.h>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<sys/socket.h>)
#include <sys/socket.h>
#endif
#if __has_include(<unistd.h>)
#include <unistd.h>
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

#if __has_include("galay-mongo/base/mongo_config.h")
#include "galay-mongo/base/mongo_config.h"
#endif
