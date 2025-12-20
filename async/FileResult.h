#ifndef GALAY_FILE_RESULT_H
#define GALAY_FILE_RESULT_H

#include "galay/common/Base.h"
#include "galay/common/Common.h"
#include "Bytes.h"
#include "galay/kernel/Engine.h"
#include <variant>
#include <coroutine>

#ifdef USE_AIO
#include <libaio.h>
#endif

namespace galay::details {

// 文件事件类型枚举
enum class FileEventType {
    Read,
    Write,
    Close
};

// FileRead参数
struct FileReadParams {
    char* buffer = nullptr;
    size_t length = 0;
    GHandle file_handle;
    Engine* engine = nullptr;
};

// FileWrite参数
struct FileWriteParams {
    Bytes bytes;
    GHandle file_handle;
    Engine* engine = nullptr;
};

// FileClose参数
struct FileCloseParams {
    GHandle file_handle;
    Engine* engine = nullptr;
};

// 文件事件参数联合体
using FileEventParams = std::variant<
    FileReadParams,
    FileWriteParams,
    FileCloseParams
>;

// 文件等待体类
template<typename ResultType>
class FileResult
{
public:
    using ptr = std::shared_ptr<FileResult>;
    using wptr = std::weak_ptr<FileResult>;

    FileResult(FileEventType type, FileEventParams params);
    ~FileResult() = default;

    // 协程接口
    bool await_ready();
    void await_suspend(std::coroutine_handle<> handle);
    std::expected<ResultType, CommonError> await_resume();

private:
    // 根据类型调用对应的ready检查
    bool checkReadReady();
    bool checkWriteReady();
    bool checkCloseReady();

    // 根据类型调用对应的suspend处理
    void handleReadSuspend(std::coroutine_handle<> handle);
    void handleWriteSuspend(std::coroutine_handle<> handle);
    void handleCloseSuspend(std::coroutine_handle<> handle);

    // 根据类型调用对应的resume处理
    std::expected<ResultType, CommonError> getReadResult();
    std::expected<ResultType, CommonError> getWriteResult();
    std::expected<ResultType, CommonError> getCloseResult();

    // 内部实现方法
    bool readBytes();
    bool writeBytes();
    void closeFile();

private:
    FileEventType m_type;
    FileEventParams m_params;
    bool m_ready = false;
    std::coroutine_handle<> m_handle;
    GHandle m_file_handle;
    Engine* m_engine = nullptr;
    std::expected<ResultType, CommonError> m_result;

#if defined(USE_IOURING)
    int m_io_result = 0;
#endif
};

// 文件事件类型别名
using FileReadResult = FileResult<Bytes>;
using FileWriteResult = FileResult<Bytes>;
using FileCloseResult = FileResult<void>;

} // namespace galay::details

#endif // GALAY_FILE_RESULT_H