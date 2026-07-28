/**
 * @file arg.hpp
 * @brief 类型安全的命令行选项与位置参数定义
 * @author galay-utils
 * @version 1.0.0
 *
 * @details `Opt<T>` 在编译期确定类型，解析后可直接 `value()` 取值，
 *          也可 `bind(&var)` 让解析结果自动写回外部变量。
 */

#ifndef GALAY_UTILS_APP_ARG_HPP
#define GALAY_UTILS_APP_ARG_HPP

#include "value.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::utils {

/// 选项与位置参数的公共接口，供解析器以类型无关方式驱动
class ArgBase {
public:
    ArgBase(std::string name, char shortName, std::string description)
        : m_name(std::move(name))
        , m_description(std::move(description))
        , m_shortName(shortName) {}

    ArgBase(const ArgBase&) = delete;
    ArgBase& operator=(const ArgBase&) = delete;
    virtual ~ArgBase() = default;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& description() const noexcept { return m_description; }
    [[nodiscard]] char shortName() const noexcept { return m_shortName; }
    [[nodiscard]] bool isRequired() const noexcept { return m_required; }
    [[nodiscard]] bool isSet() const noexcept { return m_set; }

    /// 是否为标志位（无需取值）
    [[nodiscard]] virtual bool isFlag() const noexcept = 0;
    /// 是否可重复出现并累积多个取值
    [[nodiscard]] virtual bool isMulti() const noexcept = 0;
    /// 帮助输出中的类型名
    [[nodiscard]] virtual std::string_view typeName() const noexcept = 0;
    /// 帮助输出中的默认值文本，空表示无默认值
    [[nodiscard]] virtual std::string defaultText() const = 0;
    /// 候选取值集合，空表示不限制
    [[nodiscard]] virtual const std::vector<std::string>& choices() const noexcept = 0;
    /// 从文本解析并写入，失败返回原因
    virtual std::expected<void, std::string> assign(std::string_view text) = 0;
    /// 标志位赋值，`negated` 为 `--no-xxx` 形式
    virtual std::expected<void, std::string> assignFlag(bool negated) = 0;
    /// 解析开始前重置为初始状态
    virtual void reset() = 0;
    /// 解析结束后把最终值同步到绑定变量
    virtual void flush() const = 0;

protected:
    void markSet() noexcept { m_set = true; }
    void clearSet() noexcept { m_set = false; }

    std::string m_name;
    std::string m_description;
    char m_shortName{'\0'};
    bool m_required{false};
    bool m_set{false};
};

/**
 * @brief 类型安全的命令行选项
 * @tparam T 取值类型，支持 bool、整数、浮点、`std::string`
 * @details 通过链式方法配置，解析后用 `value()` 取值；
 *          `multi()` 模式下用 `values()` 取全部出现过的取值。
 */
template<typename T>
class Opt final : public ArgBase {
public:
    using ValueType = T;

    Opt(std::string name, char shortName, std::string description)
        : ArgBase(std::move(name), shortName, std::move(description)) {}

    /// 设置默认值
    Opt& def(T value) {
        m_default = std::move(value);
        m_value = *m_default;
        return *this;
    }

    /// 标记为必选
    Opt& required(bool value = true) {
        m_required = value;
        return *this;
    }

    /// 允许重复出现并累积取值
    Opt& multi(bool value = true) {
        m_multi = value;
        return *this;
    }

    /// 限定候选取值集合
    Opt& choices(std::vector<std::string> values) {
        m_choices = std::move(values);
        return *this;
    }

    /// 绑定外部变量，解析完成后自动写回
    Opt& bind(T* target) {
        m_bound = target;
        return *this;
    }

    /// 绑定外部 vector，`multi()` 模式下写回全部取值
    Opt& bindAll(std::vector<T>* target) {
        m_boundAll = target;
        return *this;
    }

    /// 取当前值：命令行未提供时为默认值
    [[nodiscard]] const T& value() const noexcept { return m_value; }
    /// 取全部取值，仅 `multi()` 模式下有多个元素
    [[nodiscard]] const std::vector<T>& values() const noexcept { return m_values; }

    [[nodiscard]] bool isFlag() const noexcept override { return std::is_same_v<T, bool>; }
    [[nodiscard]] bool isMulti() const noexcept override { return m_multi; }
    [[nodiscard]] std::string_view typeName() const noexcept override { return CliValue<T>::typeName(); }
    [[nodiscard]] const std::vector<std::string>& choices() const noexcept override { return m_choices; }

    [[nodiscard]] std::string defaultText() const override {
        if (!m_default.has_value()) {
            return {};
        }
        return CliValue<T>::toString(*m_default);
    }

    std::expected<void, std::string> assign(std::string_view text) override {
        auto parsed = CliValue<T>::parse(text);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        if (!m_choices.empty()) {
            const std::string normalized = CliValue<T>::toString(*parsed);
            bool allowed = false;
            for (const auto& choice : m_choices) {
                if (choice == normalized || choice == text) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return std::unexpected(std::string("not in choices"));
            }
        }
        store(std::move(*parsed));
        return {};
    }

    std::expected<void, std::string> assignFlag(bool negated) override {
        if constexpr (std::is_same_v<T, bool>) {
            store(!negated);
            return {};
        } else {
            return std::unexpected(std::string("option requires a value"));
        }
    }

    void reset() override {
        clearSet();
        m_values.clear();
        m_value = m_default.value_or(T{});
    }

    void flush() const override {
        if (m_bound != nullptr) {
            *m_bound = m_value;
        }
        if (m_boundAll != nullptr) {
            *m_boundAll = m_values;
        }
    }

private:
    void store(T parsed) {
        m_value = parsed;
        m_values.push_back(std::move(parsed));
        markSet();
    }

    T m_value{};
    std::optional<T> m_default{};
    std::vector<T> m_values{};
    std::vector<std::string> m_choices{};
    T* m_bound{nullptr};
    std::vector<T>* m_boundAll{nullptr};
    bool m_multi{false};
};

} // namespace galay::utils

#endif // GALAY_UTILS_APP_ARG_HPP
