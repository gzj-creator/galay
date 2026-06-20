/**
 * @file app.hpp
 * @brief 命令行参数解析框架
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供命令行参数解析功能，支持长选项、短选项、子命令、
 *          类型转换、默认值、必选参数和标志位等特性。
 */

#ifndef GALAY_UTILS_APP_HPP
#define GALAY_UTILS_APP_HPP

#include "../common/defn.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace galay::utils {

/// 参数类型枚举
enum class ArgType { Bool, Int, Float, Double, String };

/**
 * @brief 命令行参数值封装
 * @details 支持多种类型的参数值存储和类型转换。
 */
class ArgValue {
public:
    /// 参数值类型
    using Value = std::variant<bool, int, float, double, std::string>;

    ArgValue() : m_value(false), m_set(false) {}
    explicit ArgValue(Value val) : m_value(std::move(val)), m_set(true) {}

    template<typename T>
    T as() const {
        if constexpr (std::is_same_v<T, bool>) {
            if (auto* v = std::get_if<bool>(&m_value)) return *v;
            if (auto* v = std::get_if<int>(&m_value)) return *v != 0;
            if (auto* v = std::get_if<std::string>(&m_value)) {
                return *v == "true" || *v == "1" || *v == "yes";
            }
        } else if constexpr (std::is_same_v<T, int>) {
            if (auto* v = std::get_if<int>(&m_value)) return *v;
            if (auto* v = std::get_if<bool>(&m_value)) return *v ? 1 : 0;
            if (auto* v = std::get_if<float>(&m_value)) return static_cast<int>(*v);
            if (auto* v = std::get_if<double>(&m_value)) return static_cast<int>(*v);
            if (auto* v = std::get_if<std::string>(&m_value)) return std::stoi(*v);
        } else if constexpr (std::is_same_v<T, float>) {
            if (auto* v = std::get_if<float>(&m_value)) return *v;
            if (auto* v = std::get_if<double>(&m_value)) return static_cast<float>(*v);
            if (auto* v = std::get_if<int>(&m_value)) return static_cast<float>(*v);
            if (auto* v = std::get_if<std::string>(&m_value)) return std::stof(*v);
        } else if constexpr (std::is_same_v<T, double>) {
            if (auto* v = std::get_if<double>(&m_value)) return *v;
            if (auto* v = std::get_if<float>(&m_value)) return static_cast<double>(*v);
            if (auto* v = std::get_if<int>(&m_value)) return static_cast<double>(*v);
            if (auto* v = std::get_if<std::string>(&m_value)) return std::stod(*v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (auto* v = std::get_if<std::string>(&m_value)) return *v;
            std::ostringstream oss;
            std::visit([&oss](auto&& arg) { oss << arg; }, m_value);
            return oss.str();
        }
        return T{};
    }

    bool isSet() const { return m_set; }
    void set(Value val) { m_value = std::move(val); m_set = true; }

private:
    Value m_value;
    bool m_set;
};

/**
 * @brief 命令行参数定义
 * @details 支持链式调用配置参数属性，包括长名、短名、类型、默认值等。
 */
class Arg {
public:
    Arg() = default;

    /**
     * @brief 构造参数定义
     * @param longName 长选项名
     * @param description 参数描述
     */
    Arg(std::string longName, std::string description)
        : m_longName(std::move(longName))
        , m_description(std::move(description))
        , m_type(ArgType::String)
        , m_required(false)
        , m_isFlag(false) {}

    Arg& shortName(char c) { m_shortName = c; return *this; } ///< 设置短选项名
    Arg& type(ArgType t) { m_type = t; return *this; } ///< 设置参数类型
    Arg& required(bool r = true) { m_required = r; return *this; } ///< 设置为必选参数
    Arg& defaultValue(ArgValue::Value val) { m_default = ArgValue(std::move(val)); return *this; } ///< 设置默认值
    Arg& flag(bool f = true) { m_isFlag = f; m_type = ArgType::Bool; return *this; } ///< 设置为标志位

    const std::string& longName() const { return m_longName; } ///< 获取长选项名
    char shortName() const { return m_shortName; } ///< 获取短选项名
    const std::string& description() const { return m_description; } ///< 获取描述
    ArgType type() const { return m_type; } ///< 获取参数类型
    bool isRequired() const { return m_required; } ///< 是否为必选参数
    bool isFlag() const { return m_isFlag; } ///< 是否为标志位
    const ArgValue& defaultValue() const { return m_default; } ///< 获取默认值

private:
    std::string m_longName;
    char m_shortName = '\0';
    std::string m_description;
    ArgType m_type;
    bool m_required;
    bool m_isFlag;
    ArgValue m_default;
};

/**
 * @brief 命令或子命令定义
 * @details 支持添加参数、子命令和回调函数，构成命令树。
 */
class Cmd {
public:
    /// 命令回调函数类型
    using Callback = std::function<int(Cmd&)>;

    explicit Cmd(std::string name, std::string description = "")
        : m_name(std::move(name))
        , m_description(std::move(description)) {}

    virtual ~Cmd() = default;

    Cmd& addArg(Arg arg) {
        if (arg.shortName() != '\0') {
            m_shortNameMap[arg.shortName()] = arg.longName();
        }
        m_args[arg.longName()] = std::move(arg);
        return *this;
    }

    Cmd& addSubcommand(std::unique_ptr<Cmd> cmd) {
        auto& ref = *cmd;
        m_subcommands[cmd->name()] = std::move(cmd);
        return ref;
    }

    Cmd& callback(Callback cb) {
        m_callback = std::move(cb);
        return *this;
    }

    const ArgValue& get(const std::string& name) const {
        auto it = m_values.find(name);
        if (it != m_values.end()) {
            return it->second;
        }
        auto argIt = m_args.find(name);
        if (argIt != m_args.end()) {
            return argIt->second.defaultValue();
        }
        static ArgValue empty;
        return empty;
    }

    template<typename T>
    T getAs(const std::string& name) const {
        return get(name).as<T>();
    }

    bool has(const std::string& name) const {
        auto it = m_values.find(name);
        return it != m_values.end() && it->second.isSet();
    }

    const std::vector<std::string>& positional() const { return m_positional; }
    const std::string& name() const { return m_name; }
    const std::string& description() const { return m_description; }

    void printHelp() const {
        std::cout << "Usage: " << m_name;
        if (!m_subcommands.empty()) {
            std::cout << " [command]";
        }
        std::cout << " [options]" << std::endl;

        if (!m_description.empty()) {
            std::cout << "\n" << m_description << std::endl;
        }

        if (!m_subcommands.empty()) {
            std::cout << "\nCommands:" << std::endl;
            for (const auto& [name, cmd] : m_subcommands) {
                std::cout << "  " << name;
                if (!cmd->description().empty()) {
                    std::cout << "\t" << cmd->description();
                }
                std::cout << std::endl;
            }
        }

        if (!m_args.empty()) {
            std::cout << "\nOptions:" << std::endl;
            for (const auto& [name, arg] : m_args) {
                std::cout << "  ";
                if (arg.shortName() != '\0') {
                    std::cout << "-" << arg.shortName() << ", ";
                }
                std::cout << "--" << name;
                if (!arg.isFlag()) {
                    std::cout << " <value>";
                }
                std::cout << "\t" << arg.description();
                if (arg.isRequired()) {
                    std::cout << " (required)";
                }
                std::cout << std::endl;
            }
        }
    }

protected:
    friend class App;

    bool parse(int argc, char* argv[], int startIndex = 1) {
        for (int i = startIndex; i < argc; ++i) {
            std::string arg = argv[i];

            if (!arg.empty() && arg[0] != '-') {
                auto it = m_subcommands.find(arg);
                if (it != m_subcommands.end()) {
                    m_activeSubcommand = it->second.get();
                    return m_activeSubcommand->parse(argc, argv, i + 1);
                }
            }

            if (arg.substr(0, 2) == "--") {
                std::string name = arg.substr(2);
                std::string value;

                auto eqPos = name.find('=');
                if (eqPos != std::string::npos) {
                    value = name.substr(eqPos + 1);
                    name = name.substr(0, eqPos);
                }

                if (name == "help") {
                    printHelp();
                    return false;
                }

                auto argIt = m_args.find(name);
                if (argIt == m_args.end()) {
                    std::cerr << "Unknown option: --" << name << std::endl;
                    return false;
                }

                if (argIt->second.isFlag()) {
                    m_values[name].set(true);
                } else {
                    if (value.empty() && i + 1 < argc) {
                        value = argv[++i];
                    }
                    if (!parseValue(name, value, argIt->second.type())) {
                        return false;
                    }
                }
            } else if (arg.size() >= 2 && arg[0] == '-') {
                for (size_t j = 1; j < arg.size(); ++j) {
                    char c = arg[j];
                    auto shortIt = m_shortNameMap.find(c);
                    if (shortIt == m_shortNameMap.end()) {
                        std::cerr << "Unknown option: -" << c << std::endl;
                        return false;
                    }

                    const std::string& name = shortIt->second;
                    auto argIt = m_args.find(name);

                    if (argIt->second.isFlag()) {
                        m_values[name].set(true);
                    } else {
                        std::string value;
                        if (j + 1 < arg.size()) {
                            value = arg.substr(j + 1);
                            j = arg.size();
                        } else if (i + 1 < argc) {
                            value = argv[++i];
                        }
                        if (!parseValue(name, value, argIt->second.type())) {
                            return false;
                        }
                    }
                }
            } else {
                m_positional.push_back(arg);
            }
        }

        for (const auto& [name, arg] : m_args) {
            if (arg.isRequired() && !has(name)) {
                std::cerr << "Missing required argument: --" << name << std::endl;
                return false;
            }
        }

        return true;
    }

    bool parseValue(const std::string& name, const std::string& value, ArgType type) {
        try {
            switch (type) {
                case ArgType::Bool:
                    m_values[name].set(value == "true" || value == "1" || value == "yes");
                    break;
                case ArgType::Int:
                    m_values[name].set(std::stoi(value));
                    break;
                case ArgType::Float:
                    m_values[name].set(std::stof(value));
                    break;
                case ArgType::Double:
                    m_values[name].set(std::stod(value));
                    break;
                case ArgType::String:
                    m_values[name].set(value);
                    break;
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Invalid value for --" << name << ": " << value << std::endl;
            return false;
        }
    }

    int execute() {
        if (m_activeSubcommand) {
            return m_activeSubcommand->execute();
        }
        if (m_callback) {
            return m_callback(*this);
        }
        return 0;
    }

    std::string m_name;
    std::string m_description;
    std::unordered_map<std::string, Arg> m_args;
    std::unordered_map<char, std::string> m_shortNameMap;
    std::unordered_map<std::string, std::unique_ptr<Cmd>> m_subcommands;
    std::unordered_map<std::string, ArgValue> m_values;
    std::vector<std::string> m_positional;
    Callback m_callback;
    Cmd* m_activeSubcommand = nullptr;
};

/**
 * @brief 顶层应用程序入口
 * @details 继承自 Cmd，提供 run 方法作为程序主入口。
 */
class App : public Cmd {
public:
    /**
     * @brief 构造应用程序
     * @param name 应用程序名称
     * @param description 应用程序描述
     */
    explicit App(std::string name, std::string description = "")
        : Cmd(std::move(name), std::move(description)) {}

    /**
     * @brief 解析命令行参数并执行
     * @param argc 参数个数
     * @param argv 参数数组
     * @return 执行返回值，解析失败返回 1
     */
    int run(int argc, char* argv[]) {
        if (!parse(argc, argv)) {
            return 1;
        }
        return execute();
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_APP_HPP
