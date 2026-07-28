/**
 * @file positional.hpp
 * @brief 命名位置参数定义
 * @author galay-utils
 * @version 1.0.0
 *
 * @details `Positional<T>` 让位置参数拥有名称、类型与帮助描述，
 *          `many()` 模式吞掉剩余全部位置参数。
 */

#ifndef GALAY_UTILS_APP_POSITIONAL_HPP
#define GALAY_UTILS_APP_POSITIONAL_HPP

#include "arg.hpp"

namespace galay::utils {

/**
 * @brief 命名位置参数
 * @tparam T 取值类型
 */
template<typename T>
class Positional final : public ArgBase {
public:
    using ValueType = T;

    Positional(std::string name, std::string description)
        : ArgBase(std::move(name), '\0', std::move(description)) {}

    /// 设置默认值
    Positional& def(T value) {
        m_default = std::move(value);
        m_value = *m_default;
        return *this;
    }

    /// 标记为必选
    Positional& required(bool value = true) {
        m_required = value;
        return *this;
    }

    /// 吞掉剩余全部位置参数
    Positional& many(bool value = true) {
        m_many = value;
        return *this;
    }

    /// 限定候选取值集合
    Positional& choices(std::vector<std::string> values) {
        m_choices = std::move(values);
        return *this;
    }

    /// 绑定外部变量
    Positional& bind(T* target) {
        m_bound = target;
        return *this;
    }

    /// 绑定外部 vector，`many()` 模式下写回全部取值
    Positional& bindAll(std::vector<T>* target) {
        m_boundAll = target;
        return *this;
    }

    [[nodiscard]] const T& value() const noexcept { return m_value; }
    [[nodiscard]] const std::vector<T>& values() const noexcept { return m_values; }

    [[nodiscard]] bool isFlag() const noexcept override { return false; }
    [[nodiscard]] bool isMulti() const noexcept override { return m_many; }
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
        m_value = *parsed;
        m_values.push_back(std::move(*parsed));
        markSet();
        return {};
    }

    std::expected<void, std::string> assignFlag(bool) override {
        return std::unexpected(std::string("positional argument requires a value"));
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
    T m_value{};
    std::optional<T> m_default{};
    std::vector<T> m_values{};
    std::vector<std::string> m_choices{};
    T* m_bound{nullptr};
    std::vector<T>* m_boundAll{nullptr};
    bool m_many{false};
};

} // namespace galay::utils

#endif // GALAY_UTILS_APP_POSITIONAL_HPP
