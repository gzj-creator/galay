/**
 * @file error.hpp
 * @brief 命令行解析错误类型
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 命令行解析全部通过返回值传播错误，不使用异常。
 *          `CliErrorCode::HelpRequested` 与 `CliErrorCode::VersionRequested`
 *          表示用户主动请求帮助/版本，属于正常终止而非失败。
 */

#ifndef GALAY_UTILS_APP_ERROR_HPP
#define GALAY_UTILS_APP_ERROR_HPP

#include <string>
#include <utility>

namespace galay::utils {

/// 命令行解析错误码
enum class CliErrorCode {
    UnknownOption,      ///< 未定义的选项
    MissingValue,       ///< 选项缺少取值
    InvalidValue,       ///< 取值无法转换为目标类型
    MissingRequired,    ///< 必选选项或必选位置参数缺失
    NotInChoices,       ///< 取值不在候选集合内
    UnknownSubcommand,  ///< 未定义的子命令
    HelpRequested,      ///< 用户请求帮助（非失败）
    VersionRequested,   ///< 用户请求版本（非失败）
};

/**
 * @brief 获取错误码对应的描述字符串
 * @param code 错误码
 * @return 静态字符串，覆盖所有枚举值
 */
inline constexpr const char* cliErrorString(CliErrorCode code) noexcept {
    switch (code) {
    case CliErrorCode::UnknownOption:
        return "unknown option";
    case CliErrorCode::MissingValue:
        return "missing value";
    case CliErrorCode::InvalidValue:
        return "invalid value";
    case CliErrorCode::MissingRequired:
        return "missing required argument";
    case CliErrorCode::NotInChoices:
        return "value not allowed";
    case CliErrorCode::UnknownSubcommand:
        return "unknown subcommand";
    case CliErrorCode::HelpRequested:
        return "help requested";
    case CliErrorCode::VersionRequested:
        return "version requested";
    }
    return "unknown cli error";
}

/**
 * @brief 命令行解析错误详情
 * @details `source` 为出错的选项名或位置参数名，`detail` 为补充说明。
 */
struct CliError {
    CliErrorCode code{CliErrorCode::UnknownOption};
    std::string source;
    std::string detail;

    /// 判断是否为帮助/版本这类正常终止
    [[nodiscard]] bool isTermination() const noexcept {
        return code == CliErrorCode::HelpRequested || code == CliErrorCode::VersionRequested;
    }

    /// 拼装可直接输出的错误信息
    [[nodiscard]] std::string message() const {
        std::string text = cliErrorString(code);
        if (!source.empty()) {
            text += ": ";
            text += source;
        }
        if (!detail.empty()) {
            text += " (";
            text += detail;
            text += ")";
        }
        return text;
    }
};

/// 构造错误对象的便捷函数
inline CliError makeCliError(CliErrorCode code, std::string source, std::string detail = {}) {
    return CliError{code, std::move(source), std::move(detail)};
}

} // namespace galay::utils

#endif // GALAY_UTILS_APP_ERROR_HPP
