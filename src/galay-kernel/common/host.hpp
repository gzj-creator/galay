/**
 * @file host.hpp
 * @brief IPv4/IPv6 套接字的网络地址包装器
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供 Host 结构体，将 IPv4 和 IPv6 套接字地址
 * 统一在单一接口之后。支持从 IP/端口字符串、原始
 * sockaddr_in/sockaddr_in6 结构体以及 sockaddr_storage 构造。
 * 用作 galay-kernel 中所有 TCP/UDP 套接字操作的寻址原语。
 */

#ifndef GALAY_KEERNEL_HOST_HPP
#define GALAY_KEERNEL_HOST_HPP

#include "defn.hpp"
#include <cstring>
#include <string>

namespace galay::kernel
{

/**
 * @brief IP 协议版本
 */
enum class IPType : uint8_t {
    IPV4 = 0,  ///< IPv4 地址
    IPV6 = 1,  ///< IPv6 地址
};

/**
 * @brief 套接字地址包装器
 * @details 将 IPv4 和 IPv6 地址统一在单一接口之后，
 *          提供用于系统调用的 sockaddr 指针访问。
 */
struct Host {
    sockaddr_storage m_addr{};  ///< 底层地址存储
    socklen_t m_addr_len = sizeof(sockaddr_storage);  ///< 存储的地址结构的实际长度

    /**
     * @brief 默认构造 IPv4 0.0.0.0:0 地址
     */
    Host() {
        std::memset(&m_addr, 0, sizeof(m_addr));
        m_addr.ss_family = AF_INET;
        m_addr_len = sizeof(sockaddr_in);
    }

    /**
     * @brief 从协议版本、IP 字符串和端口构造
     * @param proto IPv4 或 IPv6
     * @param ip 点分十进制或冒号十六进制 IP 字符串
     * @param port 主机字节序的端口号
     */
    Host(IPType proto, const std::string& ip, uint16_t port) {
        std::memset(&m_addr, 0, sizeof(m_addr));
        if (proto == IPType::IPV4) {
            sockaddr_in* addr4 = reinterpret_cast<sockaddr_in*>(&m_addr);
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(port);
            inet_pton(AF_INET, ip.c_str(), &addr4->sin_addr);
            m_addr_len = sizeof(sockaddr_in);
        } else {
            sockaddr_in6* addr6 = reinterpret_cast<sockaddr_in6*>(&m_addr);
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(port);
            inet_pton(AF_INET6, ip.c_str(), &addr6->sin6_addr);
            m_addr_len = sizeof(sockaddr_in6);
        }
    }

    /**
     * @brief 从 IPv4 sockaddr_in 构造
     * @param addr 原始 IPv4 套接字地址
     */
    Host(const sockaddr_in& addr) {
        std::memset(&m_addr, 0, sizeof(m_addr));
        std::memcpy(&m_addr, &addr, sizeof(addr));
        m_addr_len = sizeof(sockaddr_in);
    }

    /**
     * @brief 从 IPv6 sockaddr_in6 构造
     * @param addr 原始 IPv6 套接字地址
     */
    Host(const sockaddr_in6& addr) {
        std::memset(&m_addr, 0, sizeof(m_addr));
        std::memcpy(&m_addr, &addr, sizeof(addr));
        m_addr_len = sizeof(sockaddr_in6);
    }

    /**
     * @brief 从通用 sockaddr_storage 构造
     * @param addr 原始地址存储；通过检查 family 字段确定长度
     * @return 包装给定地址的 Host
     */
    static Host fromSockAddr(const sockaddr_storage& addr) {
        Host host;
        std::memcpy(&host.m_addr, &addr, sizeof(addr));
        if (addr.ss_family == AF_INET) {
            host.m_addr_len = sizeof(sockaddr_in);
        } else if (addr.ss_family == AF_INET6) {
            host.m_addr_len = sizeof(sockaddr_in6);
        }
        return host;
    }

    bool isIPv4() const { return m_addr.ss_family == AF_INET; }   ///< 检查存储的地址是否为 IPv4
    bool isIPv6() const { return m_addr.ss_family == AF_INET6; }  ///< 检查存储的地址是否为 IPv6

    /**
     * @brief 获取 IP 地址字符串
     * @return 点分十进制（IPv4）或冒号十六进制（IPv6）字符串
     */
    std::string ip() const {
        if (isIPv4()) {
            const sockaddr_in* addr4 = reinterpret_cast<const sockaddr_in*>(&m_addr);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf));
            return buf;
        } else {
            const sockaddr_in6* addr6 = reinterpret_cast<const sockaddr_in6*>(&m_addr);
            char buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &addr6->sin6_addr, buf, sizeof(buf));
            return buf;
        }
    }

    /**
     * @brief 获取主机字节序的端口号
     * @return 端口号
     */
    uint16_t port() const {
        if (isIPv4()) {
            const sockaddr_in* addr4 = reinterpret_cast<const sockaddr_in*>(&m_addr);
            return ntohs(addr4->sin_port);
        } else {
            const sockaddr_in6* addr6 = reinterpret_cast<const sockaddr_in6*>(&m_addr);
            return ntohs(addr6->sin6_port);
        }
    }

    sockaddr* sockAddr() { return reinterpret_cast<sockaddr*>(&m_addr); }              ///< 获取用于系统调用的可变 sockaddr 指针
    const sockaddr* sockAddr() const { return reinterpret_cast<const sockaddr*>(&m_addr); } ///< 获取常量 sockaddr 指针
    socklen_t* addrLen() { return &m_addr_len; }           ///< 获取用于系统调用更新的可变长度指针
    socklen_t addrLen() const { return m_addr_len; }       ///< 获取当前地址结构长度
};

} // namespace galay::kernel

#endif // GALAY_KEERNEL_HOST_HPP
