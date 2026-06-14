/**
 * @file watch_defs.hpp
 * @brief 文件系统监控事件类型与结果结构
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 FileWatchEvent（文件变更通知的位掩码枚举）、
 * 位运算符以及 FileWatchResult（完成时传递给 FileWatchAwaitable 的结果）。
 */

#ifndef GALAY_KERNEL_FILEWATCHDEFS_HPP
#define GALAY_KERNEL_FILEWATCHDEFS_HPP

#include <cstdint>
#include <string>

namespace galay::kernel
{

/**
 * @brief 文件监控事件类型
 */
enum class FileWatchEvent : uint32_t {
    None        = 0,
    Access      = 0x00000001,  ///< 文件被访问
    Modify      = 0x00000002,  ///< 文件被修改
    Attrib      = 0x00000004,  ///< 文件属性变化
    CloseWrite  = 0x00000008,  ///< 可写文件关闭
    CloseNoWrite= 0x00000010,  ///< 不可写文件关闭
    Open        = 0x00000020,  ///< 文件被打开
    MovedFrom   = 0x00000040,  ///< 文件被移出
    MovedTo     = 0x00000080,  ///< 文件被移入
    Create      = 0x00000100,  ///< 文件被创建
    Delete      = 0x00000200,  ///< 文件被删除
    DeleteSelf  = 0x00000400,  ///< 监控目标被删除
    MoveSelf    = 0x00000800,  ///< 监控目标被移动
    All         = 0x00000FFF,  ///< 所有事件
};

inline FileWatchEvent operator|(FileWatchEvent a, FileWatchEvent b) {
    return static_cast<FileWatchEvent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline FileWatchEvent operator&(FileWatchEvent a, FileWatchEvent b) {
    return static_cast<FileWatchEvent>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * @brief 文件监控结果
 */
struct FileWatchResult {
    FileWatchEvent event;       ///< 触发的事件类型
    std::string name;           ///< 相关文件名（目录监控时有效）
    bool isDir;                 ///< 是否是目录

    /**
     * @brief 检查是否包含指定事件
     * @param check 要检查的事件类型
     * @return 是否包含该事件
     */
    bool has(FileWatchEvent check) const {
        return (static_cast<uint32_t>(event) & static_cast<uint32_t>(check)) != 0;
    }
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_FILEWATCHDEFS_HPP
