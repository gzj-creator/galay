#pragma once
// Auto prelude for transitional C++23 module builds on Clang/GCC/MSVC.
// Keep third-party/system/dependency headers in global module fragment.

#if __has_include(<algorithm>)
#include <algorithm>
#endif
#if __has_include(<array>)
#include <array>
#endif
#if __has_include(<atomic>)
#include <atomic>
#endif
#if __has_include(<bit>)
#include <bit>
#endif
#if __has_include(<cassert>)
#include <cassert>
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
#if __has_include(<concurrentqueue/moodycamel/blockingconcurrentqueue.h>)
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#endif
#if __has_include(<concurrentqueue/moodycamel/concurrentqueue.h>)
#include <concurrentqueue/moodycamel/concurrentqueue.h>
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
#if __has_include(<cstdlib>)
#include <cstdlib>
#endif
#if __has_include(<cstring>)
#include <cstring>
#endif
#if __has_include(<deque>)
#include <deque>
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
#if __has_include(<memory>)
#include <memory>
#endif
#if __has_include(<optional>)
#include <optional>
#endif
#if __has_include(<string>)
#include <string>
#endif
#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<thread>)
#include <thread>
#endif
#if __has_include(<type_traits>)
#include <type_traits>
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

#if __has_include(<arpa/inet.h>)
#include <arpa/inet.h>
#endif
#if __has_include(<errno.h>)
#include <errno.h>
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#if __has_include(<libaio.h>)
#include <libaio.h>
#endif
#if __has_include(<linux/time_types.h>)
#include <linux/time_types.h>
#endif
#if __has_include(<mach/mach.h>)
#include <mach/mach.h>
#endif
#if __has_include(<netinet/in.h>)
#include <netinet/in.h>
#endif
#if __has_include(<netinet/tcp.h>)
#include <netinet/tcp.h>
#endif
#if __has_include(<poll.h>)
#include <poll.h>
#endif
#if __has_include(<sys/epoll.h>)
#include <sys/epoll.h>
#endif
#if __has_include(<sys/event.h>)
#include <sys/event.h>
#endif
#if __has_include(<sys/select.h>)
#include <sys/select.h>
#endif
#if __has_include(<sys/socket.h>)
#include <sys/socket.h>
#endif
#if __has_include(<sys/types.h>)
#include <sys/types.h>
#endif
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#endif
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif
