#ifndef GALAY_APP_HPP
#define GALAY_APP_HPP

#include <list>
#include <memory>
#include <string>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <functional>
#include <unordered_set>
#include <unordered_map>

namespace galay::args
{

class App;
class Cmd;
class Arg;

enum InputType {
    InputInt,
    InputFloat,
    InputDouble,
    InputString
};

template<typename T>
concept ArgInputAcceptType = requires()
{
    std::is_same_v<T, int> || std::is_same_v<T, float> || std::is_same_v<T, double> || \
    std::is_same_v<T, std::string> || std::is_same_v<T, uint32_t>;
};


class ArgInputType
{
public:
    ArgInputType(Arg* arg);
    void isInt();
    void isFloat();
    void isDouble();
    void isString();
private:
    Arg* m_arg;
};

class InputValue
{
public:
    InputValue(Arg* arg);
    template<ArgInputAcceptType T>
    T convertTo() const;
private:
    Arg* m_arg;
};

//short name 相同的，后面的覆盖前面的
class Arg
{
    friend class Cmd;
    friend class App;
    friend class ArgInputType;
    friend class InputValue;
public:
    using ptr = std::shared_ptr<Arg>;
    static Arg::ptr create(const std::string& name);

    Arg(const std::string& name);
    Arg& shortcut(char short_name);
    //default input type is String
    ArgInputType input(bool input);
    Arg& output(const std::string& output);
    Arg& required(bool required);
    Arg& unique(bool unique);
    Arg& success(std::function<void(Arg*)>&& callback);
    //callback [in] errmsg, replace print
    Arg& failure(std::function<void(std::string)>&& callback);
    // replace Failure's callback
    Arg& printError();

    InputValue value();
private:
    Cmd* m_cmd = nullptr;
    std::string m_name;
    std::string m_short_name;
    bool m_required = false;
    bool m_input = false;
    bool m_unique = false;
    InputType m_input_type = InputType::InputString;
    std::string m_output;
    std::optional<std::string> m_value;
    std::function<void(Arg*)> m_success_callback;
    std::function<void(std::string)> m_failure_callback;
};

struct ArgCollector
{
    Arg::ptr m_output_arg = nullptr;
    std::list<Arg::ptr> m_complete_args;
};

class Cmd
{
public:
    using uptr = std::unique_ptr<Cmd>;
    static Cmd::uptr create(const std::string& name);
    
    Cmd(const std::string& name);
    Cmd& addCmd(Cmd::uptr cmd);
    Cmd& addArg(Arg::ptr arg);
    void help(const std::string& help_str);

    void showHelp();
protected:
    bool collect(Arg::ptr arg);

    bool parse(int argc, int index, const char** argv);
protected:
    std::string m_name;

    std::unordered_set<std::string> m_required;
    std::unordered_map<std::string, Cmd::uptr> m_sub_cmds;

    std::unordered_map<std::string, Arg::ptr> m_sub_args;
    ArgCollector m_collector;
};


class App: public Cmd
{
    friend class Arg;
public:
    App(const std::string& name); 
    bool parse(int argc, const char** argv);
    void help(const std::string& help_str);

    void showHelp();
};

template <>
inline int InputValue::convertTo() const
{
    if(!m_arg->m_value.has_value()) throw std::runtime_error("no input value");
    try {
        return std::stoi(m_arg->m_value.value());
    } catch(...) {
        throw std::runtime_error("convert to int failed");
    }
    return 0;
}

template <>
inline uint32_t InputValue::convertTo() const
{
    if(!m_arg->m_value.has_value()) throw std::runtime_error("no input value");
    try {
        return std::stoi(m_arg->m_value.value());
    } catch(...) {
        throw std::runtime_error("convert to int failed");
    }
    return 0;
}

template <>
inline float InputValue::convertTo() const
{
    if(!m_arg->m_value.has_value()) throw std::runtime_error("no input value");
    try {
        return std::stof(m_arg->m_value.value());
    } catch(...) {
        throw std::runtime_error("convert to float failed");
    }
    return 0.0f;
}

template <>
inline double InputValue::convertTo() const
{
    if(!m_arg->m_value.has_value()) throw std::runtime_error("no input value");
    try {
        return std::stod(m_arg->m_value.value());
    } catch(...) {
        throw std::runtime_error("convert to double failed");
    }
    return 0.0;
}

template <>
inline std::string InputValue::convertTo() const
{
    if(!m_arg->m_value.has_value()) throw std::runtime_error("no input value");
    return m_arg->m_value.value();
}



}

#endif