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
#if __has_include("../../galay-kernel/async/tcp_socket.h")
#include "../../galay-kernel/async/tcp_socket.h"
#endif
#if __has_include("../../galay-utils/cache/ring_buffer.hpp")
#include "../../galay-utils/cache/ring_buffer.hpp"
#endif
#if __has_include("../../galay-kernel/common/error.h")
#include "../../galay-kernel/common/error.h"
#endif
#if __has_include("../../galay-kernel/common/host.hpp")
#include "../../galay-kernel/common/host.hpp"
#endif
#if __has_include("../../galay-kernel/concurrency/async_waiter.h")
#include "../../galay-kernel/concurrency/async_waiter.h"
#endif
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if __has_include("../../galay-kernel/core/io_scheduler.hpp")
#include "../../galay-kernel/core/io_scheduler.hpp"
#endif
#if __has_include("../../galay-kernel/core/timeout.hpp")
#include "../../galay-kernel/core/timeout.hpp"
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
#if __has_include("../async/client.h")
#include "../async/client.h"
#endif
#if __has_include("../async/conn_pool.h")
#include "../async/conn_pool.h"
#endif
#if __has_include("../base/mysql_config.h")
#include "../base/mysql_config.h"
#endif
#if __has_include("../base/mysql_error.h")
#include "../base/mysql_error.h"
#endif
#if __has_include("../base/mysql_log.h")
#include "../base/mysql_log.h"
#endif
#if __has_include("../base/mysql_value.h")
#include "../base/mysql_value.h"
#endif
#if __has_include("module_prelude.hpp")
#include "module_prelude.hpp"
#endif
#if __has_include("../protoc/builder.h")
#include "../protoc/builder.h"
#endif
#if __has_include("../protoc/mysql_auth.h")
#include "../protoc/mysql_auth.h"
#endif
#if __has_include("../protoc/mysql_protocol.h")
#include "../protoc/mysql_protocol.h"
#endif
#if __has_include("../sync/mysql_client.h")
#include "../sync/mysql_client.h"
#endif
