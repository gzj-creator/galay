/**
 * @file type_name.hpp
 * @brief 类型名称获取工具
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供编译期和运行时获取类型反解（demangled）名称的功能，
 *          支持 GCC 和 Clang 的 abi::__cxa_demangle。
 */

#ifndef GALAY_UTILS_TYPENAME_HPP
#define GALAY_UTILS_TYPENAME_HPP

#include <string>
#include <typeinfo>
#include <cstdlib>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif

namespace galay::utils {

/**
 * @brief 获取类型的反解名称
 * @tparam T 目标类型
 * @return 反解后的类型名称字符串
 */
template<typename T>
inline std::string getTypeName() {
    const char* name = typeid(T).name();

#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
#endif

    return std::string(name);
}

/**
 * @brief 获取对象的反解类型名称
 * @tparam T 对象类型
 * @param obj 目标对象
 * @return 反解后的类型名称字符串
 */
template<typename T>
inline std::string getTypeName(const T& obj) {
    const char* name = typeid(obj).name();

#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
#endif

    return std::string(name);
}

/**
 * @brief 反解符号名称
 * @param mangledName 混淆的符号名称
 * @return 反解后的符号名称字符串
 */
inline std::string demangleSymbol(const char* mangledName) {
    if (!mangledName) {
        return "";
    }

#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangledName, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
#endif

    return std::string(mangledName);
}

} // namespace galay::utils

#endif // GALAY_UTILS_TYPENAME_HPP
