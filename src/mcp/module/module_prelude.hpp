/**
 * @file module_prelude.hpp
 * @brief C++23模块构建过渡期预编译头文件
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 用于C++23模块化构建的自动预编译头文件，
 *          将第三方/系统/依赖头文件置于全局模块片段中。
 */

#pragma once

#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<charconv>)
#include <charconv>
#endif
#if __has_include(<cstdint>)
#include <cstdint>
#endif
#if __has_include(<cstdio>)
#include <cstdio>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<iostream>)
#include <iostream>
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
#if __has_include(<shared_mutex>)
#include <shared_mutex>
#endif
#if __has_include(<simdjson.h>)
#include <simdjson.h>
#endif
#if __has_include(<sstream>)
#include <sstream>
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
#if __has_include(<system_error>)
#include <system_error>
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
#if __has_include("http/client/http_client.h")
#include "http/client/http_client.h"
#endif
#if __has_include("http/server/http_router.h")
#include "http/server/http_router.h"
#endif
#if __has_include("http/server/http_server.h")
#include "http/server/http_server.h"
#endif
#if __has_include("kernel/kernel/task.h")
#include "kernel/kernel/task.h"
#endif
#if __has_include("kernel/kernel/runtime.h")
#include "kernel/kernel/runtime.h"
#endif
#if __has_include("mcp/client/http_client.h")
#include "mcp/client/http_client.h"
#endif
#if __has_include("mcp/client/stdio_client.h")
#include "mcp/client/stdio_client.h"
#endif
#if __has_include("mcp/common/mcp_base.h")
#include "mcp/common/mcp_base.h"
#endif
#if __has_include("mcp/common/mcp_error.h")
#include "mcp/common/mcp_error.h"
#endif
#if __has_include("mcp/common/mcp_json.h")
#include "mcp/common/mcp_json.h"
#endif
#if __has_include("mcp/common/json_parser.h")
#include "mcp/common/json_parser.h"
#endif
#if __has_include("mcp/common/protocol_utils.h")
#include "mcp/common/protocol_utils.h"
#endif
#if __has_include("mcp/common/schema_builder.h")
#include "mcp/common/schema_builder.h"
#endif
#if __has_include("mcp/module/module_prelude.hpp")
#include "mcp/module/module_prelude.hpp"
#endif
#if __has_include("mcp/server/http_server.h")
#include "mcp/server/http_server.h"
#endif
#if __has_include("mcp/server/stdio_server.h")
#include "mcp/server/stdio_server.h"
#endif
