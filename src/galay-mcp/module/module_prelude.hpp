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
#if __has_include("galay-http/client/http_client.h")
#include "galay-http/client/http_client.h"
#endif
#if __has_include("galay-http/server/http_router.h")
#include "galay-http/server/http_router.h"
#endif
#if __has_include("galay-http/server/http_server.h")
#include "galay-http/server/http_server.h"
#endif
#if __has_include("galay-kernel/core/task.h")
#include "galay-kernel/core/task.h"
#endif
#if __has_include("galay-kernel/core/runtime.h")
#include "galay-kernel/core/runtime.h"
#endif
#if __has_include("galay-mcp/client/client.h")
#include "galay-mcp/client/client.h"
#endif
#if __has_include("galay-mcp/common/mcp_base.h")
#include "galay-mcp/common/mcp_base.h"
#endif
#if __has_include("galay-mcp/common/mcp_error.h")
#include "galay-mcp/common/mcp_error.h"
#endif
#if __has_include("galay-mcp/common/mcp_json.h")
#include "galay-mcp/common/mcp_json.h"
#endif
#if __has_include("galay-mcp/common/json_parser.h")
#include "galay-mcp/common/json_parser.h"
#endif
#if __has_include("galay-mcp/common/protocol_utils.h")
#include "galay-mcp/common/protocol_utils.h"
#endif
#if __has_include("galay-mcp/common/schema_builder.h")
#include "galay-mcp/common/schema_builder.h"
#endif
#if __has_include("galay-mcp/module/module_prelude.hpp")
#include "galay-mcp/module/module_prelude.hpp"
#endif
#if __has_include("galay-mcp/server/http_server.h")
#include "galay-mcp/server/http_server.h"
#endif
#if __has_include("galay-mcp/server/stdio_server.h")
#include "galay-mcp/server/stdio_server.h"
#endif
