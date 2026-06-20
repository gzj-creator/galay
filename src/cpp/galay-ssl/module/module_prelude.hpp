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
#if __has_include("../../galay-utils/cache/bytes.hpp")
#include "../../galay-utils/cache/bytes.hpp"
#endif
#if __has_include("../../galay-utils/cache/byte_queue_view.hpp")
#include "../../galay-utils/cache/byte_queue_view.hpp"
#endif
#if __has_include("../../galay-kernel/common/defn.hpp")
#include "../../galay-kernel/common/defn.hpp"
#endif
#if __has_include("../../galay-kernel/common/handle_option.h")
#include "../../galay-kernel/common/handle_option.h"
#endif
#if __has_include("../../galay-kernel/common/host.hpp")
#include "../../galay-kernel/common/host.hpp"
#endif
#if __has_include("../../galay-kernel/core/awaitable.h")
#include "../../galay-kernel/core/awaitable.h"
#endif
#if __has_include("../../galay-kernel/core/io_scheduler.hpp")
#include "../../galay-kernel/core/io_scheduler.hpp"
#endif
#if __has_include("../../galay-kernel/common/log_macro.h")
#include "../../galay-kernel/common/log_macro.h"
#endif
#if __has_include("../../galay-kernel/core/timeout.hpp")
#include "../../galay-kernel/core/timeout.hpp"
#endif
#if __has_include("../../galay-kernel/core/waker.h")
#include "../../galay-kernel/core/waker.h"
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
#if __has_include("../async/awaitable.h")
#include "../async/awaitable.h"
#endif
#if __has_include("../async/ssl_socket.h")
#include "../async/ssl_socket.h"
#endif
#if __has_include("../common/defn.hpp")
#include "../common/defn.hpp"
#endif
#if __has_include("../common/error.h")
#include "../common/error.h"
#endif
#if __has_include("../common/ssl_log.h")
#include "../common/ssl_log.h"
#endif
#if __has_include("../crypto/rsa.h")
#include "../crypto/rsa.h"
#endif
#if __has_include("module_prelude.hpp")
#include "module_prelude.hpp"
#endif
#if __has_include("../ssl/ssl_context.h")
#include "../ssl/ssl_context.h"
#endif
#if __has_include("../ssl/ssl_engine.h")
#include "../ssl/ssl_engine.h"
#endif
