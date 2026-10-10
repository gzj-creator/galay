/**
 * @file env.hpp
 * @brief 进程环境变量工具
 */

#ifndef GALAY_UTILS_ENV_HPP
#define GALAY_UTILS_ENV_HPP

#include <cerrno>
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <system_error>

namespace galay::utils {

/**
 * @brief 读取、设置和删除当前进程的环境变量
 * @details 直接操作系统/CRT 环境，不缓存值，也不解析 .env 文件。
 *          环境是进程共享状态；调用方必须避免与其他环境读写并发，
 *          建议在启动工作线程/协程前完成修改。本类不添加内部锁。
 */
class Env {
public:
    Env() = delete;

    /**
     * @brief 返回环境变量值的独立副本；变量不存在时返回 std::nullopt
     * @param name 名称
     * @return 非法变量名返回 std::errc::invalid_argument
     */
    [[nodiscard]] static std::expected<std::optional<std::string>, std::error_code>
    get(const std::string& name) {
        if (!valid_name(name)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const char* value = std::getenv(name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::optional<std::string>(value);
    }

    /**
     * @brief 设置环境变量；overwrite=false 时保留已有值
     * @param name 名称
     * @param value 待设置或处理的值
     * @param overwrite 是否覆盖已有环境变量
     * @return 成功时返回空值，失败时返回 std::error_code 错误
     * @details 值允许为空，但不能包含 NUL；Windows CRT 将空值视为删除。
     *          非法输入返回 invalid_argument，系统失败保留原始错误码。
     */
    [[nodiscard]] static std::expected<void, std::error_code>
    set(const std::string& name, const std::string& value, bool overwrite = true) {
        if (!valid_name(name) || value.find('\0') != std::string::npos) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
#if defined(_WIN32)
        if (!overwrite && std::getenv(name.c_str()) != nullptr) {
            return {};
        }
        const int error = ::_putenv_s(name.c_str(), value.c_str());
        if (error != 0) {
            return std::unexpected(std::error_code(error, std::generic_category()));
        }
#else
        if (::setenv(name.c_str(), value.c_str(), overwrite ? 1 : 0) != 0) {
            const int error = errno;
            return std::unexpected(std::error_code(error, std::generic_category()));
        }
#endif
        return {};
    }

    /**
     * @brief 删除环境变量；变量不存在时仍成功
     * @param name 名称
     * @return 非法输入返回 invalid_argument，系统失败保留原始错误码
     */
    [[nodiscard]] static std::expected<void, std::error_code>
    unset(const std::string& name) {
        if (!valid_name(name)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
#if defined(_WIN32)
        const int error = ::_putenv_s(name.c_str(), "");
        if (error != 0) {
            return std::unexpected(std::error_code(error, std::generic_category()));
        }
#else
        if (::unsetenv(name.c_str()) != 0) {
            const int error = errno;
            return std::unexpected(std::error_code(error, std::generic_category()));
        }
#endif
        return {};
    }

private:
    static bool valid_name(const std::string& name) {
        return !name.empty() && name.find('=') == std::string::npos &&
               name.find('\0') == std::string::npos;
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_ENV_HPP
