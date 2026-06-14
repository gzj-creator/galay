/**
 * @file module_prelude.hpp
 * @brief C++23模块构建的前置头文件
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 为过渡性的C++23模块构建(clang/GCC/MSVC)提供自动前置包含。
 *          将第三方、系统和依赖库的头文件放入全局模块片段中，
 *          确保模块构建时正确处理外部依赖。
 */

#pragma once

#if __has_include(<algorithm>)
#include <algorithm>
#endif
#if __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif
#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<cerrno>)
#include <cerrno>
#endif
#if __has_include(<chrono>)
#include <chrono>
#endif
#if __has_include(<concepts>)
#include <concepts>
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
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if __has_include(<kernel/kernel/io_scheduler.hpp>)
#include <kernel/kernel/io_scheduler.hpp>
#endif
#if __has_include(<kernel/kernel/timeout.hpp>)
#include <kernel/kernel/timeout.hpp>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<mutex>)
#include <mutex>
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
#if __has_include(<queue>)
#include <queue>
#endif
#if __has_include(<span>)
#include <span>
#endif
#if __has_include(<stdexcept>)
#include <stdexcept>
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
#if __has_include(<vector>)
#include <vector>
#endif
#if __has_include("mysql/async/client.h")
#include "mysql/async/client.h"
#endif
#if __has_include("mysql/async/conn_pool.h")
#include "mysql/async/conn_pool.h"
#endif
#if __has_include("mysql/base/mysql_config.h")
#include "mysql/base/mysql_config.h"
#endif
#if __has_include("mysql/base/mysql_error.h")
#include "mysql/base/mysql_error.h"
#endif
#if __has_include("mysql/base/mysql_log.h")
#include "mysql/base/mysql_log.h"
#endif
#if __has_include("mysql/base/mysql_value.h")
#include "mysql/base/mysql_value.h"
#endif
#if __has_include("mysql/module/module_prelude.hpp")
#include "mysql/module/module_prelude.hpp"
#endif
#if __has_include("mysql/protoc/builder.h")
#include "mysql/protoc/builder.h"
#endif
#if __has_include("mysql/protoc/mysql_auth.h")
#include "mysql/protoc/mysql_auth.h"
#endif
#if __has_include("mysql/protoc/mysql_protocol.h")
#include "mysql/protoc/mysql_protocol.h"
#endif
#if __has_include("mysql/sync/mysql_client.h")
#include "mysql/sync/mysql_client.h"
#endif
