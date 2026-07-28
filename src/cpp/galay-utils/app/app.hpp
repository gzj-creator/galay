/**
 * @file app.hpp
 * @brief 命令行参数解析框架
 * @author galay-utils
 * @version 2.0.0
 *
 * @details 类型安全的命令行解析：`opt<T>()` 在编译期确定取值类型，
 *          解析后直接 `value()` 取值或通过 `bind()` 写回外部变量。
 *          支持长短选项、`--opt=value`、`-o value`、短选项合并、`--no-flag` 取反、
 *          `--` 终止符、重复参数累积、候选集合校验、命名位置参数与多级子命令。
 *          全程零异常，解析错误在内部显式传播，并由 `run()` 转换为进程退出码。
 *
 * @code
 * using namespace galay::utils;
 *
 * App app{"mytool", "示例工具"};
 * auto& port = app.opt<int>("port", 'p', "监听端口").def(8080);
 * auto& verbose = app.flag("verbose", 'v', "详细日志");
 *
 * app.on([&](Cmd&) {
 *     serve(port.value(), verbose.value());
 *     return 0;
 * });
 * return app.run(argc, argv);
 * @endcode
 */

#ifndef GALAY_UTILS_APP_HPP
#define GALAY_UTILS_APP_HPP

#include "../common/defn.hpp"
#include "cmd.hpp"

#include <iostream>

namespace galay::utils {

/**
 * @brief 顶层应用程序入口
 * @details 继承自 `Cmd`，额外提供版本号声明与 `run()` 主入口。
 */
class App : public Cmd {
public:
    explicit App(std::string name, std::string description = "")
        : Cmd(std::move(name), std::move(description)) {}

    /// 声明版本文本，声明后 `--version` 生效
    App& version(std::string text) {
        m_versionText = std::move(text);
        return *this;
    }

    /**
     * @brief 解析参数并执行选中命令的回调
     * @param argc 参数个数
     * @param argv 参数数组
     * @param out 帮助与版本输出流
     * @param err 错误输出流
     * @return 回调返回值；帮助/版本返回 0；解析失败返回 1
     */
    int run(int argc, const char* const* argv, std::ostream& out = std::cout,
            std::ostream& err = std::cerr) {
        auto parsed = parse(argc, argv, 1);
        if (parsed) {
            return execute();
        }

        const CliError& error = parsed.error();
        if (error.code == CliErrorCode::HelpRequested) {
            selected().printHelp(out);
            return 0;
        }
        if (error.code == CliErrorCode::VersionRequested) {
            out << error.source << '\n';
            out.flush();
            return 0;
        }

        err << m_name << ": " << error.message() << '\n';
        err.flush();
        selected().printHelp(err);
        return 1;
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_APP_HPP
