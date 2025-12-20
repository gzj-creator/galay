#include "FileResult.h"
#include "galay/kernel/Waker.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

namespace galay::details {

// FileResult构造函数
template<typename ResultType>
FileResult<ResultType>::FileResult(FileEventType type, FileEventParams params)
    : m_type(type), m_params(params)
{
    // 根据类型提取对应的参数和句柄
    std::visit([this](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, FileReadParams>) {
            m_engine = arg.engine;
            m_file_handle = arg.file_handle;
        } else if constexpr (std::is_same_v<T, FileWriteParams>) {
            m_engine = arg.engine;
            m_file_handle = arg.file_handle;
        } else if constexpr (std::is_same_v<T, FileCloseParams>) {
            m_engine = arg.engine;
            m_file_handle = arg.file_handle;
        }
    }, m_params);
}

// await_ready实现
template<typename ResultType>
bool FileResult<ResultType>::await_ready()
{
    switch (m_type) {
        case FileEventType::Read:
            return checkReadReady();
        case FileEventType::Write:
            return checkWriteReady();
        case FileEventType::Close:
            return checkCloseReady();
        default:
            return false;
    }
}

// await_suspend实现
template<typename ResultType>
void FileResult<ResultType>::await_suspend(std::coroutine_handle<> handle)
{
    m_handle = handle;

    switch (m_type) {
        case FileEventType::Read:
            handleReadSuspend(handle);
            break;
        case FileEventType::Write:
            handleWriteSuspend(handle);
            break;
        case FileEventType::Close:
            handleCloseSuspend(handle);
            break;
    }
}

// await_resume实现
template<typename ResultType>
std::expected<ResultType, CommonError> FileResult<ResultType>::await_resume()
{
    switch (m_type) {
        case FileEventType::Read:
            return getReadResult();
        case FileEventType::Write:
            return getWriteResult();
        case FileEventType::Close:
            return getCloseResult();
        default:
            return std::unexpected(CommonError{1, "Unknown FileEventType"});
    }
}

// Read相关实现
template<typename ResultType>
bool FileResult<ResultType>::checkReadReady()
{
    return readBytes();
}

template<typename ResultType>
void FileResult<ResultType>::handleReadSuspend(std::coroutine_handle<> handle)
{
    if (!m_engine) {
        m_result = std::unexpected(CommonError{1, "Engine is null"});
        handle.resume();
        return;
    }

    // 创建waker并注册到引擎
    auto waker = Waker([handle]() {
        handle.resume();
    });

    // 注册读事件到引擎
    WakerWrapper wrapper;
    wrapper.addType(WakerType::READ, std::move(waker));

    m_engine->addWakers(&wrapper, WakerType::READ, waker, m_file_handle, nullptr);
}

template<typename ResultType>
std::expected<ResultType, CommonError> FileResult<ResultType>::getReadResult()
{
    return m_result;
}

template<typename ResultType>
bool FileResult<ResultType>::readBytes()
{
    auto* params = std::get_if<FileReadParams>(&m_params);
    if (!params || !params->buffer) {
        m_result = std::unexpected(CommonError{1, "Invalid file read params"});
        return true;
    }

    // 设置为非阻塞模式
    int flags = fcntl(m_file_handle.fd, F_GETFL, 0);
    fcntl(m_file_handle.fd, F_SETFL, flags | O_NONBLOCK);

    ssize_t bytes = read(m_file_handle.fd, params->buffer, params->length);
    if (bytes > 0) {
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            m_result = Bytes(params->buffer, bytes);
        }
        return true;
    }

    if (bytes == 0) {
        // EOF
        m_result = std::unexpected(CommonError{0, "EOF reached"});
        return true;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false; // 需要等待
    }

    m_result = std::unexpected(CommonError{static_cast<uint32_t>(errno), "File read failed"});
    return true;
}

// Write相关实现
template<typename ResultType>
bool FileResult<ResultType>::checkWriteReady()
{
    return writeBytes();
}

template<typename ResultType>
void FileResult<ResultType>::handleWriteSuspend(std::coroutine_handle<> handle)
{
    if (!m_engine) {
        m_result = std::unexpected(CommonError{1, "Engine is null"});
        handle.resume();
        return;
    }

    auto waker = Waker([handle]() {
        handle.resume();
    });

    WakerWrapper wrapper;
    wrapper.addType(WakerType::WRITE, std::move(waker));

    m_engine->addWakers(&wrapper, WakerType::WRITE, waker, m_file_handle, nullptr);
}

template<typename ResultType>
std::expected<ResultType, CommonError> FileResult<ResultType>::getWriteResult()
{
    return m_result;
}

template<typename ResultType>
bool FileResult<ResultType>::writeBytes()
{
    auto* params = std::get_if<FileWriteParams>(&m_params);
    if (!params) {
        m_result = std::unexpected(CommonError{1, "Invalid file write params"});
        return true;
    }

    // 设置为非阻塞模式
    int flags = fcntl(m_file_handle.fd, F_GETFL, 0);
    fcntl(m_file_handle.fd, F_SETFL, flags | O_NONBLOCK);

    ssize_t bytes = write(m_file_handle.fd, params->bytes.data(), params->bytes.size());
    if (bytes >= 0) {
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            m_result = Bytes(params->bytes.data(), bytes);
        }
        return true;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
    }

    m_result = std::unexpected(CommonError{static_cast<uint32_t>(errno), "File write failed"});
    return true;
}

// Close相关实现
template<typename ResultType>
bool FileResult<ResultType>::checkCloseReady()
{
    // close操作通常是同步的，不需要等待
    closeFile();
    return true;
}

template<typename ResultType>
void FileResult<ResultType>::handleCloseSuspend(std::coroutine_handle<> handle)
{
    // close不应该suspend，直接完成
    closeFile();
    handle.resume();
}

template<typename ResultType>
std::expected<ResultType, CommonError> FileResult<ResultType>::getCloseResult()
{
    if constexpr (std::is_same_v<ResultType, void>) {
        return {};
    } else {
        return m_result;
    }
}

template<typename ResultType>
void FileResult<ResultType>::closeFile()
{
    if (close(m_file_handle.fd) == 0) {
        if constexpr (std::is_same_v<ResultType, void>) {
            m_result = std::expected<ResultType, CommonError>{};
        } else {
            m_result = std::unexpected(CommonError{1, "Close completed"});
        }
    } else {
        m_result = std::unexpected(CommonError{static_cast<uint32_t>(errno), "File close failed"});
    }
}

// 显式实例化常用的模板
template class FileResult<void>;
template class FileResult<Bytes>;

} // namespace galay::details