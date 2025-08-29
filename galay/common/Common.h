#ifndef GALAY_COMMON_H
#define GALAY_COMMON_H

#include <openssl/ssl.h>
#include <concepts>
#include <pthread.h>
#include "Error.h"
#include "Base.h"

namespace galay 
{

#define LL long long

    inline void setThreadName(const std::string& name) {
    #ifdef __linux__
        pthread_setname_np(pthread_self(), name.c_str());
    #elif defined(_WIN32)
        if (auto SetThreadDescription = 
            reinterpret_cast<decltype(&::SetThreadDescription)>(
                GetProcAddress(GetModuleHandle("kernel32.dll"), "SetThreadDescription"))) {
            wchar_t wname[256];
            mbstowcs(wname, name, 256);
            SetThreadDescription(GetCurrentThread(), wname);
        }
    #elif defined(__APPLE__)
        pthread_setname_np(name.c_str());
    #else
        // 其他平台或不支持
    #endif
    }


    enum FamilyType
    {
        IPV4,
        IPV6,
    };

    struct Host
    {
        std::string ip;
        uint32_t port = 0;
    };

    struct SockAddr
    {
        FamilyType type = IPV4;
        Host host;
    };

    bool initializeSSLServerEnv(const char* cert_file, const char* key_file);
    bool initialiszeSSLClientEnv(const char* server_pem = nullptr);
    bool destroySSLEnv();

    bool initializeHttp2ServerEnv(const char* cert_file, const char* key_file);
    bool initializeHttp2ClientEnv(const char* server_pem = nullptr);
    bool destroyHttp2Env();

    SSL_CTX* getGlobalSSLCtx();

    class HandleOption
    {
    public:
        using error_ptr = error::Error::ptr;
        explicit HandleOption(GHandle handle);
        bool handleBlock();
        bool handleNonBlock();
        bool handleReuseAddr();
        bool handleReusePort();
        error_ptr getError();
    private:
        GHandle m_handle;
        error_ptr m_error;
    };

    

    template<typename T>
    concept ValueTypeTrait = requires(T t) {
        std::is_default_constructible_v<T> && !std::is_void_v<T> && std::is_move_assignable_v<T>;
    };

    template<ValueTypeTrait T>
    class ValueWrapper {
        template<ValueTypeTrait U> 
        friend void makeValue(ValueWrapper<U>& wrapper, U&& value, error::Error::ptr error);
        template<ValueTypeTrait U> 
        friend void makeValue(ValueWrapper<U>& wrapper, error::Error::ptr error);
    public:
        using error_ptr = error::Error::ptr;
        ValueWrapper() {}
        T moveValue() { return std::move(m_value);}
        bool success() const { return m_error == nullptr; }
        error_ptr getError() { return m_error; }
    private:
        T m_value;
        error_ptr m_error = nullptr;
    };

    template<ValueTypeTrait T>
    inline void makeValue(ValueWrapper<T>& wrapper, T&& value, error::Error::ptr error) {
        wrapper.m_value = std::move(value);
        wrapper.m_error = error;
    }

    template<ValueTypeTrait T>
    inline void makeValue(ValueWrapper<T>& wrapper, error::Error::ptr error) {
        wrapper.m_error = error;
    }

}

#endif