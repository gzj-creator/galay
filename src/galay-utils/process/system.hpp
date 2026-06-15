#ifndef GALAY_UTILS_SYSTEM_HPP
#define GALAY_UTILS_SYSTEM_HPP

#include "galay-utils/common/defn.hpp"
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <thread>

#if defined(GALAY_PLATFORM_MACOS) || defined(GALAY_PLATFORM_LINUX) || defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <netdb.h>
#include <arpa/inet.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#elif defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace galay::utils {

/**
 * @file system.hpp
 * @brief 系统工具类
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供跨平台的系统工具函数，包括文件操作、环境变量管理、
 *          网络地址解析和系统信息查询。时间相关能力位于 core/time.hpp。
 */

/**
 * @brief 系统工具类
 * @details 提供文件、环境变量、网络和系统信息等静态工具方法。
 */
class System {
public:
    // File functions

    static std::optional<std::string> readFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return std::nullopt;
        }

        // 获取文件大小
        std::streamsize size = file.tellg();
        if (size < 0) {
            return std::nullopt;
        }

        file.seekg(0, std::ios::beg);

        std::string content;
        content.resize(static_cast<size_t>(size));

        if (file.read(content.data(), size)) {
            return content;
        }

        return std::nullopt;
    }

    static bool writeFile(const std::string& path, const std::string& content, bool append = false) {
        std::ios_base::openmode mode = std::ios::binary;
        if (append) {
            mode |= std::ios::app;
        } else {
            mode |= std::ios::trunc;
        }

        std::ofstream file(path, mode);
        if (!file) {
            return false;
        }

        file.write(content.data(), content.size());
        return file.good();
    }

    static std::optional<std::string> readFileMmap(const std::string& path) {
#if defined(_WIN32)
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            return std::nullopt;
        }

        if (fileSize.QuadPart == 0) {
            CloseHandle(hFile);
            return std::string();
        }

        HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMapping) {
            CloseHandle(hFile);
            return std::nullopt;
        }

        void* data = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (!data) {
            CloseHandle(hMapping);
            CloseHandle(hFile);
            return std::nullopt;
        }

        std::string result(static_cast<const char*>(data), fileSize.QuadPart);

        UnmapViewOfFile(data);
        CloseHandle(hMapping);
        CloseHandle(hFile);

        return result;
#else
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return std::nullopt;
        }

        struct stat st;
        if (fstat(fd, &st) < 0) {
            close(fd);
            return std::nullopt;
        }

        if (st.st_size == 0) {
            close(fd);
            return std::string();
        }

        void* data = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return std::nullopt;
        }

        std::string result(static_cast<const char*>(data), st.st_size);

        munmap(data, st.st_size);
        close(fd);

        return result;
#endif
    }

    static bool fileExists(const std::string& path) {
#if defined(_WIN32)
        DWORD attrs = GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES;
#else
        struct stat st;
        return stat(path.c_str(), &st) == 0;
#endif
    }

    static bool isDirectory(const std::string& path) {
#if defined(_WIN32)
        DWORD attrs = GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }

    static int64_t fileSize(const std::string& path) {
#if defined(_WIN32)
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
            return -1;
        }
        LARGE_INTEGER size;
        size.HighPart = fad.nFileSizeHigh;
        size.LowPart = fad.nFileSizeLow;
        return size.QuadPart;
#else
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            return -1;
        }
        return st.st_size;
#endif
    }

    static bool createDirectory(const std::string& path) {
#if defined(_WIN32)
        return CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
        std::string current;
        for (size_t i = 0; i < path.length(); ++i) {
            current += path[i];
            if (path[i] == '/' || i == path.length() - 1) {
                if (!current.empty() && current != "/") {
                    if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                        return false;
                    }
                }
            }
        }
        return true;
#endif
    }

    static bool remove(const std::string& path) {
#if defined(_WIN32)
        if (isDirectory(path)) {
            return RemoveDirectoryA(path.c_str()) != 0;
        }
        return DeleteFileA(path.c_str()) != 0;
#else
        return ::remove(path.c_str()) == 0;
#endif
    }

    static std::vector<std::string> listDirectory(const std::string& path) {
        std::vector<std::string> result;

#if defined(_WIN32)
        WIN32_FIND_DATAA findData;
        std::string searchPath = path + "\\*";
        HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::string name = findData.cFileName;
                if (name != "." && name != "..") {
                    result.push_back(name);
                }
            } while (FindNextFileA(hFind, &findData));
            FindClose(hFind);
        }
#else
        DIR* dir = opendir(path.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name != "." && name != "..") {
                    result.push_back(name);
                }
            }
            closedir(dir);
        }
#endif

        return result;
    }

    // Environment functions

    static std::string getEnv(const std::string& name, const std::string& defaultValue = "") {
        const char* value = std::getenv(name.c_str());
        return value ? std::string(value) : defaultValue;
    }

    static bool setEnv(const std::string& name, const std::string& value, bool overwrite = true) {
#if defined(_WIN32)
        if (!overwrite && std::getenv(name.c_str()) != nullptr) {
            return true;
        }
        return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
        return setenv(name.c_str(), value.c_str(), overwrite ? 1 : 0) == 0;
#endif
    }

    static bool unsetEnv(const std::string& name) {
#if defined(_WIN32)
        return _putenv_s(name.c_str(), "") == 0;
#else
        return unsetenv(name.c_str()) == 0;
#endif
    }

    // Network functions

    static std::string resolveHostIPv4(const std::string& hostname) {
        struct addrinfo hints{}, *result;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0) {
            return "";
        }

        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

        freeaddrinfo(result);
        return std::string(ip);
    }

    static std::string resolveHostIPv6(const std::string& hostname) {
        struct addrinfo hints{}, *result;
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0) {
            return "";
        }

        char ip[INET6_ADDRSTRLEN];
        struct sockaddr_in6* addr = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
        inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));

        freeaddrinfo(result);
        return std::string(ip);
    }

    /// 地址类型枚举
    enum class AddressType { Invalid, IPv4, IPv6, Domain };

    /**
     * @brief 检查地址类型
     * @param address 待检查的地址字符串
     * @return 地址类型枚举值
     */
    static AddressType checkAddressType(const std::string& address) {
        struct in_addr ipv4;
        struct in6_addr ipv6;

        if (inet_pton(AF_INET, address.c_str(), &ipv4) == 1) {
            return AddressType::IPv4;
        }

        if (inet_pton(AF_INET6, address.c_str(), &ipv6) == 1) {
            return AddressType::IPv6;
        }

        for (char c : address) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') {
                return AddressType::Invalid;
            }
        }

        if (!address.empty() && address.find('.') != std::string::npos) {
            return AddressType::Domain;
        }

        return AddressType::Invalid;
    }

    // System info

    static unsigned int cpuCount() {
        return std::thread::hardware_concurrency();
    }

    static std::string hostname() {
        char buffer[256];
#if defined(_WIN32)
        DWORD size = sizeof(buffer);
        if (GetComputerNameA(buffer, &size)) {
            return std::string(buffer);
        }
#else
        if (gethostname(buffer, sizeof(buffer)) == 0) {
            return std::string(buffer);
        }
#endif
        return "";
    }

    static std::string currentDir() {
#if defined(_WIN32)
        char buffer[MAX_PATH];
        if (_getcwd(buffer, sizeof(buffer))) {
            return std::string(buffer);
        }
#else
        char buffer[PATH_MAX];
        if (getcwd(buffer, sizeof(buffer))) {
            return std::string(buffer);
        }
#endif
        return "";
    }

    static bool changeDir(const std::string& path) {
#if defined(_WIN32)
        return _chdir(path.c_str()) == 0;
#else
        return chdir(path.c_str()) == 0;
#endif
    }

    static std::string executablePath() {
#if defined(__APPLE__)
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0) {
            char realPath[PATH_MAX];
            if (realpath(buffer, realPath)) {
                return std::string(realPath);
            }
            return std::string(buffer);
        }
#elif defined(__linux__)
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';
            return std::string(buffer);
        }
#elif defined(_WIN32)
        char buffer[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
        if (len > 0 && len < sizeof(buffer)) {
            return std::string(buffer);
        }
#endif
        return "";
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_SYSTEM_HPP
