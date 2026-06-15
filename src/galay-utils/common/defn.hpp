/**
 * @file defn.hpp
 * @brief 公共类型定义和平台检测宏
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 定义平台检测宏、架构检测宏、编译器检测宏、常用类型别名、
 *          智能指针别名、不可拷贝/不可移动基类和单例基类等基础设施。
 */

#ifndef GALAY_UTILS_DEFN_HPP
#define GALAY_UTILS_DEFN_HPP

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <string_view>

/// 平台检测宏
#if defined(__APPLE__)
    #define GALAY_PLATFORM_MACOS 1
#elif defined(__linux__)
    #define GALAY_PLATFORM_LINUX 1
#elif defined(_WIN32) || defined(_WIN64)
    #define GALAY_PLATFORM_WINDOWS 1
#endif

/// 架构检测宏
#if defined(__x86_64__) || defined(_M_X64)
    #define GALAY_ARCH_X64 1
#elif defined(__i386__) || defined(_M_IX86)
    #define GALAY_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define GALAY_ARCH_ARM64 1
#endif

/// 编译器检测宏
#if defined(__clang__)
    #define GALAY_COMPILER_CLANG 1
#elif defined(__GNUC__)
    #define GALAY_COMPILER_GCC 1
#elif defined(_MSC_VER)
    #define GALAY_COMPILER_MSVC 1
#endif

/// 分支预测提示宏
#if defined(GALAY_COMPILER_GCC) || defined(GALAY_COMPILER_CLANG)
    #define GALAY_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define GALAY_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define GALAY_LIKELY(x)   (x)
    #define GALAY_UNLIKELY(x) (x)
#endif

/// 强制内联宏
#if defined(GALAY_COMPILER_MSVC)
    #define GALAY_FORCE_INLINE __forceinline
#else
    #define GALAY_FORCE_INLINE __attribute__((always_inline)) inline
#endif

/// 未使用变量消除宏
#define GALAY_UNUSED(x) (void)(x)

namespace galay::utils {

/// 通用类型别名
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using f32 = float;
using f64 = double;

using usize = size_t;
using isize = ptrdiff_t;

/// 智能指针别名
template<typename T>
using UniquePtr = std::unique_ptr<T>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

/// 函数类型别名
template<typename T>
using Func = std::function<T>;

/// 字符串类型别名
using String = std::string;
using StringView = std::string_view;

/**
 * @brief 不可拷贝基类
 * @details 继承此类的对象不可被拷贝构造和拷贝赋值。
 */
class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

/**
 * @brief 不可移动基类
 * @details 继承此类的对象不可被移动构造和移动赋值。
 */
class NonMovable {
protected:
    NonMovable() = default;
    ~NonMovable() = default;

    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

/**
 * @brief 单例基类
 * @details 继承此类的对象将获得线程安全的单例模式。
 * @tparam T 单例类型
 */
template<typename T>
class Singleton : public NonCopyable, public NonMovable {
public:
    /**
     * @brief 获取单例实例
     * @return 单例的引用
     */
    static T& instance() {
        static T instance;
        return instance;
    }

protected:
    Singleton() = default;
    ~Singleton() = default;
};

} // namespace galay::utils

#endif // GALAY_UTILS_DEFN_HPP
