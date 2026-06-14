#pragma once
// Auto prelude for transitional C++23 module builds on Clang/GCC/MSVC.
// Keep third-party/system/dependency headers in global module fragment.

#if __has_include(<algorithm>)
#include <algorithm>
#endif
#if __has_include(<arm_neon.h>)
#include <arm_neon.h>
#endif
#if __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif
#if __has_include(<array>)
#include <array>
#endif
#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<cctype>)
#include <cctype>
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
#if __has_include(<ctime>)
#include <ctime>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#if __has_include(<filesystem>)
#include <filesystem>
#endif
#if __has_include(<format>)
#include <format>
#endif
#if __has_include(<fstream>)
#include <fstream>
#endif
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<utils/encoding/base64.hpp>)
#include <utils/encoding/base64.hpp>
#endif
#if __has_include(<iomanip>)
#include <iomanip>
#endif
#if __has_include(<locale>)
#include <locale>
#endif
#if __has_include(<map>)
#include <map>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<queue>)
#include <queue>
#endif
#if __has_include(<random>)
#include <random>
#endif
#if __has_include(<regex>)
#include <regex>
#endif
#if __has_include(<set>)
#include <set>
#endif
#if __has_include(<sstream>)
#include <sstream>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<sys/stat.h>)
#include <sys/stat.h>
#endif
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#endif
#if __has_include(<system_error>)
#include <system_error>
#endif
#if __has_include(<time.h>)
#include <time.h>
#endif
#if __has_include(<type_traits>)
#include <type_traits>
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
#if __has_include("http/client/http_client.h")
#include "http/client/http_client.h"
#endif
#if __has_include("http/common/http_log.h")
#include "http/common/http_log.h"
#endif
#if __has_include("http/common/iovec_utils.h")
#include "http/common/iovec_utils.h"
#endif
#if __has_include("http/kernel/http_conn.h")
#include "http/kernel/http_conn.h"
#endif
#if __has_include("http/kernel/http_reader.h")
#include "http/kernel/http_reader.h"
#endif
#if __has_include("http/server/http_router.h")
#include "http/server/http_router.h"
#endif
#if __has_include("http/plugin/common/defn.h")
#include "http/plugin/common/defn.h"
#endif
#if __has_include("http/plugin/common/conn_info_storage.hpp")
#include "http/plugin/common/conn_info_storage.hpp"
#endif
#if __has_include("http/plugin/blacklist/blacklist.hpp")
#include "http/plugin/blacklist/blacklist.hpp"
#endif
#if __has_include("http/server/http_server.h")
#include "http/server/http_server.h"
#endif
#if __has_include("http/kernel/http_session.h")
#include "http/kernel/http_session.h"
#endif
#if __has_include("http/kernel/http_writer.h")
#include "http/kernel/http_writer.h"
#endif
#if __has_include("http2/client/h2_client.h")
#include "http2/client/h2_client.h"
#endif
#if __has_include("http2/client/h2c_client.h")
#include "http2/client/h2c_client.h"
#endif
#if __has_include("http2/kernel/http2_conn.h")
#include "http2/kernel/http2_conn.h"
#endif
#if __has_include("http2/server/http2_server.h")
#include "http2/server/http2_server.h"
#endif
#if __has_include("http2/kernel/http2_stream.h")
#include "http2/kernel/http2_stream.h"
#endif
#if __has_include("http2/kernel/stream_mgr.h")
#include "http2/kernel/stream_mgr.h"
#endif
#if __has_include("ws/client/ws_client.h")
#include "ws/client/ws_client.h"
#endif
#if __has_include("ws/kernel/ws_conn.h")
#include "ws/kernel/ws_conn.h"
#endif
#if __has_include("ws/kernel/ws_heartbeat.h")
#include "ws/kernel/ws_heartbeat.h"
#endif
#if __has_include("ws/kernel/ws_reader.h")
#include "ws/kernel/ws_reader.h"
#endif
#if __has_include("ws/kernel/reader_cfg.h")
#include "ws/kernel/reader_cfg.h"
#endif
#if __has_include("ws/client/ws_session.h")
#include "ws/client/ws_session.h"
#endif
#if __has_include("ws/server/ws_upgrade.h")
#include "ws/server/ws_upgrade.h"
#endif
#if __has_include("ws/kernel/ws_writer.h")
#include "ws/kernel/ws_writer.h"
#endif
#if __has_include("ws/kernel/writer_cfg.h")
#include "ws/kernel/writer_cfg.h"
#endif
#if __has_include("http2/module/module_prelude.hpp")
#include "http2/module/module_prelude.hpp"
#endif
#if __has_include("http/protoc/http_base.h")
#include "http/protoc/http_base.h"
#endif
#if __has_include("http/protoc/http_body.h")
#include "http/protoc/http_body.h"
#endif
#if __has_include("http/protoc/http_chunk.h")
#include "http/protoc/http_chunk.h"
#endif
#if __has_include("http/protoc/http_error.h")
#include "http/protoc/http_error.h"
#endif
#if __has_include("http/protoc/http_header.h")
#include "http/protoc/http_header.h"
#endif
#if __has_include("http/protoc/http_request.h")
#include "http/protoc/http_request.h"
#endif
#if __has_include("http/protoc/http_response.h")
#include "http/protoc/http_response.h"
#endif
#if __has_include("http2/protoc/http2_base.h")
#include "http2/protoc/http2_base.h"
#endif
#if __has_include("http2/protoc/http2_error.h")
#include "http2/protoc/http2_error.h"
#endif
#if __has_include("http2/protoc/http2_frame.h")
#include "http2/protoc/http2_frame.h"
#endif
#if __has_include("http2/builder/http2_frame_builder.h")
#include "http2/builder/http2_frame_builder.h"
#endif
#if __has_include("http2/protoc/http2_hpack.h")
#include "http2/protoc/http2_hpack.h"
#endif
#if __has_include("http2/utils/h2_helper.h")
#include "http2/utils/h2_helper.h"
#endif
#if __has_include("ws/protoc/ws_error.h")
#include "ws/protoc/ws_error.h"
#endif
#if __has_include("ws/protoc/ws_frame.h")
#include "ws/protoc/ws_frame.h"
#endif
#if __has_include("ws/builder/ws_frame_builder.h")
#include "ws/builder/ws_frame_builder.h"
#endif
#if __has_include("ws/utils/ws_helper.h")
#include "ws/utils/ws_helper.h"
#endif
#if __has_include("ws/protoc/ws_base.h")
#include "ws/protoc/ws_base.h"
#endif
#if __has_include("http/builder/http_builder.h")
#include "http/builder/http_builder.h"
#endif
#if __has_include("http/utils/http_helper.h")
#include "http/utils/http_helper.h"
#endif
#if __has_include("kernel/async/tcp_socket.h")
#include "kernel/async/tcp_socket.h"
#endif
#if __has_include(<utils/cache/ring_buffer.hpp>)
#include <utils/cache/ring_buffer.hpp>
#endif
#if __has_include("kernel/common/error.h")
#include "kernel/common/error.h"
#endif
#if __has_include("kernel/common/sleep.hpp")
#include "kernel/common/sleep.hpp"
#endif
#if __has_include("kernel/concurrency/async_waiter.h")
#include "kernel/concurrency/async_waiter.h"
#endif
#if __has_include("kernel/concurrency/unsafe_channel.h")
#include "kernel/concurrency/unsafe_channel.h"
#endif
#if __has_include("kernel/kernel/awaitable.h")
#include "kernel/kernel/awaitable.h"
#endif
#if __has_include("kernel/kernel/task.h")
#include "kernel/kernel/task.h"
#endif
#if __has_include("kernel/kernel/io_handlers.hpp")
#include "kernel/kernel/io_handlers.hpp"
#endif
#if __has_include("kernel/kernel/runtime.h")
#include "kernel/kernel/runtime.h"
#endif
#if __has_include("kernel/kernel/timeout.hpp")
#include "kernel/kernel/timeout.hpp"
#endif
#if __has_include("ssl/async/ssl_socket.h")
#include "ssl/async/ssl_socket.h"
#endif
#if __has_include("ssl/ssl/ssl_context.h")
#include "ssl/ssl/ssl_context.h"
#endif
