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

    bool initializeHttp2ServerEnv(const char* cert_file, const char* key_file)
    {
        if(!initializeSSLServerEnv(cert_file, key_file)) {
            return false;
        }
        const unsigned char alpn_protocols[] = "\x08\x04\x00\x00"; // HTTP/2
        SSL_CTX_set_alpn_protos(SslCtx, alpn_protocols, sizeof(alpn_protocols));
        return true;
    }

    bool initializeHttp2ClientEnv(const char* server_pem)
    {
        if(!initialiszeSSLClientEnv(server_pem)) {
            return false;
        }
        const unsigned char alpn_protocols[] = "\x08\x04\x00\x00"; // HTTP/2
        SSL_CTX_set_alpn_protos(SslCtx, alpn_protocols, sizeof(alpn_protocols));
        return true;
    }

    bool destroyHttp2Env()
    {
        return destroySSLEnv();
    }


    SSL_CTX *getGlobalSSLCtx()
    {
        return SslCtx;
    }

    HandleOption::HandleOption(const GHandle handle)
        : m_handle(handle), m_error(nullptr)
    {
    }

    bool HandleOption::handleBlock()
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
            m_error = std::make_shared<SystemError>(error::ErrorCode::Error_SetBlockError, errno);
            return false;
        }
        return true;
    }

    bool HandleOption::handleNonBlock()
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
            m_error = std::make_shared<SystemError>(error::ErrorCode::Error_SetNoBlockError, errno);
            return false;
        }
        return true;
    }

    bool HandleOption::handleReuseAddr()
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
            m_error = std::make_shared<SystemError>(error::ErrorCode::Error_SetSockOptError, errno);
            return false;
        }
        return true;
    }

    bool HandleOption::handleReusePort()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        constexpr int option = 1;
        if (const int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option)); ret < 0) {
            m_error = std::make_shared<SystemError>(error::ErrorCode::Error_SetSockOptError, errno);
            return false;
        }
    #elif  defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        //To Do
    #endif
        return true;
    }

    HandleOption::error_ptr HandleOption::getError()
    {
        return m_error;
    }



}