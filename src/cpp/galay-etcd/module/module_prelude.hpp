/**
 * @file module_prelude.hpp
 * @brief galay-etcd 模块统一头文件包含前置
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 作为 galay-etcd 模块的总入口头文件，使用 __has_include 条件编译
 *          按需包含标准库头文件和项目内部头文件。
 *          外部使用方只需包含此文件即可获得 galay-etcd 的完整 API。
 */

#pragma once

#if __has_include(<chrono>)
#include <chrono>
#endif
#if __has_include(<cstdint>)
#include <cstdint>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<vector>)
#include <vector>
#endif

#if __has_include("../../galay-http/kernel/http_session.h")
#include "../../galay-http/kernel/http_session.h"
#endif
#if __has_include("../../galay-kernel/async/tcp_socket.h")
#include "../../galay-kernel/async/tcp_socket.h"
#endif
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#if __has_include("../../galay-kernel/core/io_scheduler.hpp")
#include "../../galay-kernel/core/io_scheduler.hpp"
#endif
#if __has_include("../../galay-kernel/core/runtime.h")
#include "../../galay-kernel/core/runtime.h"
#endif
#if __has_include("../../galay-utils/encoding/base64.hpp")
#include "../../galay-utils/encoding/base64.hpp"
#endif

#if __has_include("../base/etcd_config.h")
#include "../base/etcd_config.h"
#endif
#if __has_include("../base/etcd_error.h")
#include "../base/etcd_error.h"
#endif
#if __has_include("../base/etcd_log.h")
#include "../base/etcd_log.h"
#endif
#if __has_include("../base/etcd_value.h")
#include "../base/etcd_value.h"
#endif
#if __has_include("../async/client.h")
#include "../async/client.h"
#endif
#if __has_include("../sync/etcd_client.h")
#include "../sync/etcd_client.h"
#endif
