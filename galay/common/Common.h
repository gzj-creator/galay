#ifndef GALAY_COMMON_H
#define GALAY_COMMON_H

#include <openssl/ssl.h>
#include <concepts>
#include <pthread.h>
#include <expected>
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

    using namespace error;
    class HandleOption
    {
    public:
        explicit HandleOption(GHandle handle);
        std::expected<void, CommonError> handleBlock();
        std::expected<void, CommonError> handleNonBlock();
        std::expected<void, CommonError> handleReuseAddr();
        std::expected<void, CommonError> handleReusePort();
    private:
        GHandle m_handle;
    };
}

#endif