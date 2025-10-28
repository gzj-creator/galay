#include "Common.h"
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
#ifdef __linux__
     #include <sys/sendfile.h>
    #include <sys/eventfd.h>
#endif
#elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
    #include <ws2ipdef.h>
    #include <WS2tcpip.h>
#endif

namespace galay 
{ 


    SSL_CTX* SslCtx = nullptr;
    // API

    bool initializeSSLServerEnv(const char *cert_file, const char *key_file)
    {
        if(SslCtx != nullptr) {
            return false;
        }
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        SslCtx = SSL_CTX_new(TLS_server_method());
        if (SslCtx == nullptr) {
            return false;
        }

        // 加载证书和私钥
        if (SSL_CTX_use_certificate_file(SslCtx, cert_file, SSL_FILETYPE_PEM) <= 0) {
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(SslCtx, key_file, SSL_FILETYPE_PEM) <= 0) {
            return false;
        }
        return true;
    }

    bool initialiszeSSLClientEnv(const char* server_pem)
    {
        if(SslCtx != nullptr) {
            return false;
        }
        SslCtx = SSL_CTX_new(TLS_client_method());
        if (SslCtx == nullptr) {
            return false;
        }
        if(server_pem) {
            if(SSL_CTX_load_verify_locations(SslCtx, server_pem, NULL) <= 0) {
                return false;
            }
        }
        return true;
    }

    bool destroySSLEnv()
    {
        SSL_CTX_free(SslCtx);
        EVP_cleanup();
        return true;
    }


    SSL_CTX *getGlobalSSLCtx()
    {
        return SslCtx;
    }

    HandleOption::HandleOption(const GHandle handle)
        : m_handle(handle)
    {
    }

    std::expected<void, CommonError> HandleOption::handleBlock()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        int flag = fcntl(m_handle.fd, F_GETFL, 0);
        flag &= ~O_NONBLOCK;
        int ret = fcntl(m_handle.fd, F_SETFL, flag);
    #elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        u_long mode = 0; // 1 表示非阻塞模式
        int ret = ioctlsocket(m_handle, FIONBIO, &mode);
    #endif
        if (ret < 0) {
            return std::unexpected(CommonError{CallSetBlockError, static_cast<uint32_t>(errno)});
        }
        return {};
    }

    std::expected<void, CommonError> HandleOption::handleNonBlock()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        int flag = fcntl(m_handle.fd, F_GETFL, 0);
        flag |= O_NONBLOCK;
        int ret = fcntl(m_handle.fd, F_SETFL, flag);
    #elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        u_long mode = 1; // 1 表示非阻塞模式
        int ret = ioctlsocket(m_handle.fd, FIONBIO, &mode);
    #endif
        if (ret < 0) {
            return std::unexpected(CommonError(CallSetNoBlockError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> HandleOption::handleReuseAddr()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        int option = 1;
        int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    #elif  defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        BOOL option = TRUE;
        int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEADDR, (char*)&option, sizeof(option));
    #endif
        if (ret < 0) {
            return std::unexpected(CommonError(CallSetSockOptError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> HandleOption::handleReusePort()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        constexpr int option = 1;
        if (const int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option)); ret < 0) {
            return std::unexpected(CommonError(CallSetSockOptError, static_cast<uint32_t>(errno)));
        }
    #elif  defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        //To Do
    #endif
        return {};
    }
}