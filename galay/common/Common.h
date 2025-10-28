#ifndef GALAY_COMMON_H
#define GALAY_COMMON_H

#include <openssl/ssl.h>
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


    /**
     * @brief 地址族类型枚举
     */
    enum FamilyType
    {
        IPV4,  ///< IPv4地址族
        IPV6,  ///< IPv6地址族
    };

    /**
     * @brief 主机地址结构
     */
    struct Host
    {
        std::string ip;      ///< IP地址字符串
        uint32_t port = 0;   ///< 端口号
    };

    /**
     * @brief Socket地址结构
     */
    struct SockAddr
    {
        FamilyType type = IPV4;  ///< 地址族类型
        Host host;               ///< 主机地址
    };


    using namespace error;
    
    /**
     * @brief 句柄选项配置类
     * @details 提供对socket句柄的各种选项配置功能
     */
    class HandleOption
    {
    public:
        /**
         * @brief 构造函数
         * @param handle 要配置的句柄
         */
        explicit HandleOption(GHandle handle);
        
        /**
         * @brief 设置句柄为阻塞模式
         * @return 成功返回void，失败返回CommonError
         */
        std::expected<void, CommonError> handleBlock();
        
        /**
         * @brief 设置句柄为非阻塞模式
         * @return 成功返回void，失败返回CommonError
         */
        std::expected<void, CommonError> handleNonBlock();
        
        /**
         * @brief 设置地址重用选项（SO_REUSEADDR）
         * @return 成功返回void，失败返回CommonError
         */
        std::expected<void, CommonError> handleReuseAddr();
        
        /**
         * @brief 设置端口重用选项（SO_REUSEPORT）
         * @return 成功返回void，失败返回CommonError
         */
        std::expected<void, CommonError> handleReusePort();
    private:
        GHandle m_handle;  ///< 句柄对象
    };
}

#endif