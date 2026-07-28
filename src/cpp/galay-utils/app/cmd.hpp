/**
 * @file cmd.hpp
 * @brief 命令与子命令定义、参数解析与帮助输出
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 解析全程零异常，错误通过 `std::expected<void, CliError>` 传播。
 *          选项查找基于小规模线性扫描与 `std::string_view`，避免哈希与临时字符串分配。
 */

#ifndef GALAY_UTILS_APP_CMD_HPP
#define GALAY_UTILS_APP_CMD_HPP

#include "arg.hpp"
#include "error.hpp"
#include "positional.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace galay::utils {

class Cmd;

/// 命令回调类型：返回进程退出码
using CmdCallback = std::function<int(Cmd&)>;

/**
 * @brief 命令或子命令
 * @details 通过 `opt<T>()` / `flag()` / `pos<T>()` 声明参数，`sub()` 声明子命令，
 *          `on()` 注册回调。所有声明方法返回对应对象的引用以便链式配置。
 */
class Cmd {
public:
    explicit Cmd(std::string name, std::string description = "")
        : m_name(std::move(name))
        , m_description(std::move(description)) {}

    Cmd(const Cmd&) = delete;
    Cmd& operator=(const Cmd&) = delete;
    virtual ~Cmd() = default;

    /**
     * @brief 声明一个带取值的选项
     * @tparam T 取值类型
     * @param name 长选项名，不含 `--`
     * @param shortName 短选项名，`'\0'` 表示不提供
     * @param description 帮助描述
     * @return 选项引用，可继续链式配置
     */
    template<typename T>
    Opt<T>& opt(std::string name, char shortName, std::string description = "") {
        auto owned = std::make_unique<Opt<T>>(std::move(name), shortName, std::move(description));
        auto& ref = *owned;
        m_options.push_back(std::move(owned));
        return ref;
    }

    /// 声明一个无短名的选项
    template<typename T>
    Opt<T>& opt(std::string name, std::string description = "") {
        return opt<T>(std::move(name), '\0', std::move(description));
    }

    /**
     * @brief 声明一个布尔标志位
     * @details 支持 `--name` 置真与 `--no-name` 置假。
     */
    Opt<bool>& flag(std::string name, char shortName, std::string description = "") {
        return opt<bool>(std::move(name), shortName, std::move(description)).def(false);
    }

    /// 声明一个无短名的标志位
    Opt<bool>& flag(std::string name, std::string description = "") {
        return flag(std::move(name), '\0', std::move(description));
    }

    /**
     * @brief 声明一个命名位置参数
     * @tparam T 取值类型
     * @param name 参数名，仅用于帮助与错误信息
     * @param description 帮助描述
     */
    template<typename T>
    Positional<T>& pos(std::string name, std::string description = "") {
        auto owned = std::make_unique<Positional<T>>(std::move(name), std::move(description));
        auto& ref = *owned;
        m_positionals.push_back(std::move(owned));
        return ref;
    }

    /**
     * @brief 声明一个子命令
     * @param name 子命令名
     * @param description 帮助描述
     * @return 子命令引用
     */
    Cmd& sub(std::string name, std::string description = "") {
        auto owned = std::make_unique<Cmd>(std::move(name), std::move(description));
        auto& ref = *owned;
        m_subcommands.push_back(std::move(owned));
        return ref;
    }

    /// 注册命令回调
    Cmd& on(CmdCallback callback) {
        m_callback = std::move(callback);
        return *this;
    }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& description() const noexcept { return m_description; }

    /// 未被命名位置参数消费的剩余参数
    [[nodiscard]] const std::vector<std::string>& rest() const noexcept { return m_rest; }

    /// 实际被选中的最深层子命令，无子命令时为自身
    [[nodiscard]] Cmd& selected() noexcept {
        return m_activeSub != nullptr ? m_activeSub->selected() : *this;
    }

    /// 判断某个选项是否在命令行中出现过
    [[nodiscard]] bool has(std::string_view name) const noexcept {
        const ArgBase* found = findLong(name);
        return found != nullptr && found->isSet();
    }

    /// 输出帮助信息，顺序与声明顺序一致
    void printHelp(std::ostream& out) const;

protected:
    /// 解析参数数组，成功返回空，失败或请求帮助返回 `CliError`
    std::expected<void, CliError> parse(int argc, const char* const* argv, int startIndex);

    /// 执行选中命令的回调
    [[nodiscard]] int execute() {
        if (m_activeSub != nullptr) {
            return m_activeSub->execute();
        }
        return m_callback ? m_callback(*this) : 0;
    }

    std::string m_name;
    std::string m_description;
    std::string m_versionText;
    std::vector<std::unique_ptr<ArgBase>> m_options;
    std::vector<std::unique_ptr<ArgBase>> m_positionals;
    std::vector<std::unique_ptr<Cmd>> m_subcommands;
    std::vector<std::string> m_rest;
    CmdCallback m_callback;
    Cmd* m_activeSub{nullptr};

private:
    /// 按长名查找选项
    [[nodiscard]] ArgBase* findLong(std::string_view name) const noexcept {
        for (const auto& option : m_options) {
            if (option->name() == name) {
                return option.get();
            }
        }
        return nullptr;
    }

    /// 按短名查找选项
    [[nodiscard]] ArgBase* findShort(char shortName) const noexcept {
        if (shortName == '\0') {
            return nullptr;
        }
        for (const auto& option : m_options) {
            if (option->shortName() == shortName) {
                return option.get();
            }
        }
        return nullptr;
    }

    /// 按名查找子命令
    [[nodiscard]] Cmd* findSub(std::string_view name) const noexcept {
        for (const auto& sub : m_subcommands) {
            if (sub->name() == name) {
                return sub.get();
            }
        }
        return nullptr;
    }

    /// 解析前重置所有状态，保证可重复解析
    void resetState() {
        m_rest.clear();
        m_activeSub = nullptr;
        for (const auto& option : m_options) {
            option->reset();
        }
        for (const auto& positional : m_positionals) {
            positional->reset();
        }
    }

    /// 解析结束后回写绑定变量
    void flushBindings() const {
        for (const auto& option : m_options) {
            option->flush();
        }
        for (const auto& positional : m_positionals) {
            positional->flush();
        }
    }

    std::expected<void, CliError> handleLong(std::string_view token, int argc, const char* const* argv, int& index);
    std::expected<void, CliError> handleShort(std::string_view token, int argc, const char* const* argv, int& index);
    std::expected<void, CliError> acceptPositional(std::string_view token);
    [[nodiscard]] std::expected<void, CliError> checkRequired() const;

    std::size_t m_positionalCursor{0};
};

inline std::expected<void, CliError>
Cmd::handleLong(std::string_view token, int argc, const char* const* argv, int& index) {
    std::string_view body = token.substr(2);
    std::string_view inlineValue;
    bool hasInlineValue = false;

    const auto eq = body.find('=');
    if (eq != std::string_view::npos) {
        inlineValue = body.substr(eq + 1);
        body = body.substr(0, eq);
        hasInlineValue = true;
    }

    if (body == "help") {
        return std::unexpected(makeCliError(CliErrorCode::HelpRequested, m_name));
    }
    if (body == "version" && !m_versionText.empty()) {
        return std::unexpected(makeCliError(CliErrorCode::VersionRequested, m_versionText));
    }

    ArgBase* option = findLong(body);
    bool negated = false;
    if (option == nullptr && body.starts_with("no-")) {
        ArgBase* candidate = findLong(body.substr(3));
        if (candidate != nullptr && candidate->isFlag()) {
            option = candidate;
            negated = true;
        }
    }
    if (option == nullptr) {
        return std::unexpected(makeCliError(CliErrorCode::UnknownOption, std::string("--").append(body)));
    }

    const std::string reported = std::string("--").append(option->name());

    if (option->isFlag() && !hasInlineValue) {
        auto assigned = option->assignFlag(negated);
        if (!assigned) {
            return std::unexpected(makeCliError(CliErrorCode::InvalidValue, reported, std::move(assigned.error())));
        }
        return {};
    }

    if (!hasInlineValue) {
        if (index + 1 >= argc) {
            return std::unexpected(makeCliError(CliErrorCode::MissingValue, reported));
        }
        inlineValue = argv[++index];
    }

    auto assigned = option->assign(inlineValue);
    if (!assigned) {
        const auto code = assigned.error() == "not in choices" ? CliErrorCode::NotInChoices : CliErrorCode::InvalidValue;
        return std::unexpected(makeCliError(code, reported, std::string(inlineValue)));
    }
    return {};
}

inline std::expected<void, CliError>
Cmd::handleShort(std::string_view token, int argc, const char* const* argv, int& index) {
    for (std::size_t i = 1; i < token.size(); ++i) {
        const char letter = token[i];
        if (letter == 'h') {
            if (findShort('h') == nullptr) {
                return std::unexpected(makeCliError(CliErrorCode::HelpRequested, m_name));
            }
        }

        ArgBase* option = findShort(letter);
        if (option == nullptr) {
            return std::unexpected(makeCliError(CliErrorCode::UnknownOption, std::string("-").append(1, letter)));
        }

        const std::string reported = std::string("-").append(1, letter);

        if (option->isFlag()) {
            auto assigned = option->assignFlag(false);
            if (!assigned) {
                return std::unexpected(makeCliError(CliErrorCode::InvalidValue, reported, std::move(assigned.error())));
            }
            continue;
        }

        std::string_view value;
        if (i + 1 < token.size()) {
            value = token.substr(token[i + 1] == '=' ? i + 2 : i + 1);
            i = token.size();
        } else if (index + 1 < argc) {
            value = argv[++index];
        } else {
            return std::unexpected(makeCliError(CliErrorCode::MissingValue, reported));
        }

        auto assigned = option->assign(value);
        if (!assigned) {
            const auto code = assigned.error() == "not in choices" ? CliErrorCode::NotInChoices : CliErrorCode::InvalidValue;
            return std::unexpected(makeCliError(code, reported, std::string(value)));
        }
    }
    return {};
}

inline std::expected<void, CliError> Cmd::acceptPositional(std::string_view token) {
    while (m_positionalCursor < m_positionals.size()) {
        ArgBase* target = m_positionals[m_positionalCursor].get();
        if (!target->isMulti() && target->isSet()) {
            ++m_positionalCursor;
            continue;
        }
        auto assigned = target->assign(token);
        if (!assigned) {
            const auto code = assigned.error() == "not in choices" ? CliErrorCode::NotInChoices : CliErrorCode::InvalidValue;
            return std::unexpected(makeCliError(code, target->name(), std::string(token)));
        }
        if (!target->isMulti()) {
            ++m_positionalCursor;
        }
        return {};
    }
    m_rest.emplace_back(token);
    return {};
}

namespace detail {

/**
 * @brief 预扫描剩余参数中是否存在帮助请求
 * @details 帮助优先于必选校验：`app sub --help` 不应先报父命令缺少必选参数。
 *          `--` 之后的内容按普通取值处理，不参与扫描。
 */
inline bool containsHelpToken(int argc, const char* const* argv, int from) noexcept {
    for (int i = from; i < argc; ++i) {
        const std::string_view token = argv[i];
        if (token == "--") {
            return false;
        }
        if (token == "--help" || token == "-h") {
            return true;
        }
    }
    return false;
}

} // namespace detail

inline std::expected<void, CliError> Cmd::checkRequired() const {
    for (const auto& option : m_options) {
        if (option->isRequired() && !option->isSet()) {
            return std::unexpected(
                makeCliError(CliErrorCode::MissingRequired, std::string("--").append(option->name())));
        }
    }
    for (const auto& positional : m_positionals) {
        if (positional->isRequired() && !positional->isSet()) {
            return std::unexpected(makeCliError(CliErrorCode::MissingRequired, positional->name()));
        }
    }
    return {};
}

inline std::expected<void, CliError> Cmd::parse(int argc, const char* const* argv, int startIndex) {
    resetState();
    m_positionalCursor = 0;
    bool endOfOptions = false;

    // 帮助优先：即使缺少必选参数，--help 也应正常输出帮助
    const bool helpRequested = detail::containsHelpToken(argc, argv, startIndex);

    for (int i = startIndex; i < argc; ++i) {
        const std::string_view token = argv[i];

        if (!endOfOptions && token == "--") {
            endOfOptions = true;
            continue;
        }

        if (!endOfOptions && token.size() > 2 && token.starts_with("--")) {
            auto handled = handleLong(token, argc, argv, i);
            if (!handled) {
                return handled;
            }
            continue;
        }

        if (!endOfOptions && token.size() >= 2 && token[0] == '-' && token != "-") {
            auto handled = handleShort(token, argc, argv, i);
            if (!handled) {
                return handled;
            }
            continue;
        }

        // 子命令名优先于位置参数匹配，否则带 many() 位置参数的命令会吞掉子命令
        if (!endOfOptions && !m_subcommands.empty()) {
            Cmd* sub = findSub(token);
            if (sub != nullptr) {
                m_activeSub = sub;
                // 子命令后跟 --help 时跳过父命令必选校验，直接交由子命令输出帮助
                if (!detail::containsHelpToken(argc, argv, i + 1)) {
                    auto required = checkRequired();
                    if (!required) {
                        return required;
                    }
                    flushBindings();
                }
                return sub->parse(argc, argv, i + 1);
            }
            if (m_positionalCursor >= m_positionals.size()) {
                return std::unexpected(makeCliError(CliErrorCode::UnknownSubcommand, std::string(token)));
            }
        }

        auto accepted = acceptPositional(token);
        if (!accepted) {
            return accepted;
        }
    }

    if (!helpRequested) {
        auto required = checkRequired();
        if (!required) {
            return required;
        }
    }
    flushBindings();
    return {};
}

namespace detail {

/// 拼装选项在帮助中的左列文本，例如 `-p, --port <int>`
inline std::string optionLabel(const ArgBase& option) {
    std::string label;
    if (option.shortName() != '\0') {
        label.append("-").append(1, option.shortName()).append(", ");
    } else {
        label.append("    ");
    }
    label.append("--").append(option.name());
    if (!option.isFlag()) {
        label.append(" <").append(option.typeName()).append(">");
    }
    return label;
}

/// 拼装选项在帮助中的右列补充说明
inline std::string optionSuffix(const ArgBase& option) {
    std::string suffix;
    if (option.isRequired()) {
        suffix.append(" [required]");
    }
    if (option.isMulti()) {
        suffix.append(" [repeatable]");
    }
    const auto& choices = option.choices();
    if (!choices.empty()) {
        suffix.append(" {");
        for (std::size_t i = 0; i < choices.size(); ++i) {
            if (i != 0) {
                suffix.append("|");
            }
            suffix.append(choices[i]);
        }
        suffix.append("}");
    }
    if (!option.isRequired()) {
        const std::string defaultText = option.defaultText();
        // 标志位默认为假是常态，不必在帮助里重复
        const bool noisyFlagDefault = option.isFlag() && defaultText == "false";
        if (!defaultText.empty() && !noisyFlagDefault) {
            suffix.append(" (default: ").append(defaultText).append(")");
        }
    }
    return suffix;
}

/// 按最长左列文本补齐空格
inline void writeRow(std::ostream& out, const std::string& label, std::size_t width,
                     const std::string& description, const std::string& suffix) {
    out << "  " << label;
    if (label.size() < width) {
        out << std::string(width - label.size(), ' ');
    }
    out << "  " << description << suffix << '\n';
}

} // namespace detail

inline void Cmd::printHelp(std::ostream& out) const {
    out << "Usage: " << m_name;
    if (!m_subcommands.empty()) {
        out << " <command>";
    }
    out << " [options]";
    for (const auto& positional : m_positionals) {
        const bool optional = !positional->isRequired();
        out << ' ' << (optional ? "[" : "<") << positional->name()
            << (positional->isMulti() ? "..." : "") << (optional ? "]" : ">");
    }
    out << '\n';

    if (!m_description.empty()) {
        out << '\n' << m_description << '\n';
    }

    std::size_t width = 0;
    for (const auto& option : m_options) {
        width = std::max(width, detail::optionLabel(*option).size());
    }
    for (const auto& positional : m_positionals) {
        width = std::max(width, positional->name().size());
    }
    for (const auto& sub : m_subcommands) {
        width = std::max(width, sub->name().size());
    }

    const bool hasHelpShort = findShort('h') != nullptr;
    const std::string helpLabel = hasHelpShort ? "    --help" : "-h, --help";
    width = std::max(width, helpLabel.size());

    if (!m_subcommands.empty()) {
        out << "\nCommands:\n";
        for (const auto& sub : m_subcommands) {
            detail::writeRow(out, sub->name(), width, sub->description(), {});
        }
    }

    if (!m_positionals.empty()) {
        out << "\nArguments:\n";
        for (const auto& positional : m_positionals) {
            detail::writeRow(out, positional->name(), width, positional->description(),
                             detail::optionSuffix(*positional));
        }
    }

    out << "\nOptions:\n";
    for (const auto& option : m_options) {
        detail::writeRow(out, detail::optionLabel(*option), width, option->description(),
                         detail::optionSuffix(*option));
    }
    detail::writeRow(out, helpLabel, width, "show this help", {});
    if (!m_versionText.empty()) {
        detail::writeRow(out, "    --version", width, "show version", {});
    }
    out.flush();
}

} // namespace galay::utils

#endif // GALAY_UTILS_APP_CMD_HPP
