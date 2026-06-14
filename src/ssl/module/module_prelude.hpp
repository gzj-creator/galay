#pragma once
// Auto prelude for transitional C++23 module builds on Clang/GCC/MSVC.
// Keep third-party/system/dependency headers in global module fragment.

#if __has_include(<algorithm>)
#include <algorithm>
#endif
#if __has_include(<cerrno>)
#include <cerrno>
#endif
#if __has_include(<coroutine>)
#include <coroutine>
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
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<format>)
#include <format>
#endif
#if __has_include(<utils/cache/bytes.hpp>)
#include <utils/cache/bytes.hpp>
#endif
#if __has_include(<utils/cache/byte_queue_view.hpp>)
#include <utils/cache/byte_queue_view.hpp>
#endif
#if __has_include(<kernel/common/defn.hpp>)
#include <kernel/common/defn.hpp>
#endif
#if __has_include(<kernel/common/handle_option.h>)
#include <kernel/common/handle_option.h>
#endif
#if __has_include(<kernel/common/host.hpp>)
#include <kernel/common/host.hpp>
#endif
#if __has_include(<kernel/kernel/awaitable.h>)
#include <kernel/kernel/awaitable.h>
#endif
#if __has_include(<kernel/kernel/io_scheduler.hpp>)
#include <kernel/kernel/io_scheduler.hpp>
#endif
#if __has_include(<kernel/common/log_macro.h>)
#include <kernel/common/log_macro.h>
#endif
#if __has_include(<kernel/kernel/timeout.hpp>)
#include <kernel/kernel/timeout.hpp>
#endif
#if __has_include(<kernel/kernel/waker.h>)
#include <kernel/kernel/waker.h>
#endif
#if __has_include(<limits>)
#include <limits>
#endif
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<netinet/in.h>)
#include <netinet/in.h>
#endif
#if __has_include(<openssl/err.h>)
#include <openssl/err.h>
#endif
#if __has_include(<openssl/ssl.h>)
#include <openssl/ssl.h>
#endif
#if __has_include(<openssl/x509.h>)
#include <openssl/x509.h>
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
#if __has_include(<sys/event.h>)
#include <sys/event.h>
#endif
#if __has_include(<sys/socket.h>)
#include <sys/socket.h>
#endif
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif
#if __has_include(<vector>)
#include <vector>
#endif
#if __has_include("ssl/async/awaitable.h")
#include "ssl/async/awaitable.h"
#endif
#if __has_include("ssl/async/ssl_socket.h")
#include "ssl/async/ssl_socket.h"
#endif
#if __has_include("ssl/common/defn.hpp")
#include "ssl/common/defn.hpp"
#endif
#if __has_include("ssl/common/error.h")
#include "ssl/common/error.h"
#endif
#if __has_include("ssl/common/ssl_log.h")
#include "ssl/common/ssl_log.h"
#endif
#if __has_include("ssl/crypto/rsa.h")
#include "ssl/crypto/rsa.h"
#endif
#if __has_include("ssl/module/module_prelude.hpp")
#include "ssl/module/module_prelude.hpp"
#endif
#if __has_include("ssl/ssl/ssl_context.h")
#include "ssl/ssl/ssl_context.h"
#endif
#if __has_include("ssl/ssl/ssl_engine.h")
#include "ssl/ssl/ssl_engine.h"
#endif
