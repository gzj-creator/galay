/**
 * @file module_prelude.hpp
 * @brief C++23 模块构建预编译头文件
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 用于 C++23 模块过渡构建，将第三方和系统头文件放入全局模块片段，
 *          避免将它们附加到命名模块的管辖范围。支持 Clang/GCC/MSVC 编译器。
 */

#pragma once

#if __has_include(<algorithm>)
#include <algorithm>
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
#if __has_include(<cerrno>)
#include <cerrno>
#endif
#if __has_include(<chrono>)
#include <chrono>
#endif
#if __has_include(<condition_variable>)
#include <condition_variable>
#endif
#if __has_include(<concepts>)
#include <concepts>
#endif
#if __has_include(<csignal>)
#include <csignal>
#endif
#if __has_include(<cstddef>)
#include <cstddef>
#endif
#if __has_include(<cstdint>)
#include <cstdint>
#endif
#if __has_include(<cstdio>)
#include <cstdio>
#endif
#if __has_include(<cstdlib>)
#include <cstdlib>
#endif
#if __has_include(<cstring>)
#include <cstring>
#endif
#if __has_include(<ctime>)
#include <ctime>
#endif
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif
#if __has_include(<direct.h>)
#include <direct.h>
#endif
#if __has_include(<dirent.h>)
#include <dirent.h>
#endif
#if __has_include(<dlfcn.h>)
#include <dlfcn.h>
#endif
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#endif
#if __has_include(<expected>)
#include <expected>
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#if __has_include(<fstream>)
#include <fstream>
#endif
#if __has_include(<functional>)
#include <functional>
#endif
#if __has_include(<future>)
#include <future>
#endif
#if __has_include(<iomanip>)
#include <iomanip>
#endif
#if __has_include(<iostream>)
#include <iostream>
#endif
#if __has_include(<limits>)
#include <limits>
#endif
#if __has_include(<mach-o/dyld.h>)
#include <mach-o/dyld.h>
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
#if __has_include(<netdb.h>)
#include <netdb.h>
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
#if __has_include(<set>)
#include <set>
#endif
#if __has_include(<shared_mutex>)
#include <shared_mutex>
#endif
#if __has_include(<signal.h>)
#include <signal.h>
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
#if __has_include(<sys/mman.h>)
#include <sys/mman.h>
#endif
#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#endif
#if __has_include(<sys/stat.h>)
#include <sys/stat.h>
#endif
#if __has_include(<sys/types.h>)
#include <sys/types.h>
#endif
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#endif
#if __has_include(<sys/wait.h>)
#include <sys/wait.h>
#endif
#if __has_include(<thread>)
#include <thread>
#endif
#if __has_include(<tlhelp32.h>)
#include <tlhelp32.h>
#endif
#if __has_include(<typeinfo>)
#include <typeinfo>
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
#if __has_include(<windows.h>)
#include <windows.h>
#endif
#if __has_include(<winsock2.h>)
#include <winsock2.h>
#endif
#if __has_include(<ws2tcpip.h>)
#include <ws2tcpip.h>
#endif
