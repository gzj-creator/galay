#ifndef GALAY_FILE_H
#define GALAY_FILE_H 

#if !defined(__WIN32__) && !defined(__WIN64__)
    #include <unistd.h>
    #include <fcntl.h>
#endif

#include "FileEvent.h"
#include "galay/kernel/runtime/Runtime.h"

namespace galay 
{
    #define DEFAULT_BUFFER_SIZE 1024

    using namespace error;

    class OpenFlags
    {
    public:
        OpenFlags& append() { m_flags |= O_APPEND; return *this; }
        OpenFlags& create() { m_flags |= O_CREAT; return *this; }
        OpenFlags& truncate() { m_flags |= O_TRUNC; return *this; }
        OpenFlags& read() { m_flags |= O_RDONLY; return *this; }
        OpenFlags& write() { m_flags |= O_WRONLY; return *this; }
        OpenFlags& readWrite() { m_flags |= O_RDWR; return *this; }
        OpenFlags& noBlock() { m_flags |= O_NONBLOCK; return *this; }
        OpenFlags& cloExec() { m_flags |= O_CLOEXEC; return *this; }

        int getFlags() const { return m_flags; }
    private:
        int m_flags = 0;
    };

    class FileModes 
    {
    public:
        FileModes() = default;  // Default creates mode 0644
        // Builder pattern for permission flags
        FileModes& userRead()      { m_mode |= S_IRUSR; return *this; }
        FileModes& userWrite()     { m_mode |= S_IWUSR; return *this; }
        FileModes& userExecute()   { m_mode |= S_IXUSR; return *this; }
        FileModes& groupRead()     { m_mode |= S_IRGRP; return *this; }
        FileModes& groupWrite()    { m_mode |= S_IWGRP; return *this; }
        FileModes& groupExecute()  { m_mode |= S_IXGRP; return *this; }
        FileModes& otherRead()     { m_mode |= S_IROTH; return *this; }
        FileModes& otherWrite()    { m_mode |= S_IWOTH; return *this; }
        FileModes& otherExecute()  { m_mode |= S_IXOTH; return *this; }
        // Higher-level permission presets
        FileModes& readOnly() { m_mode = 0; return userRead().groupRead().otherRead(); }
        FileModes& readWrite() { m_mode = 0; return userRead().userWrite().groupRead().otherRead(); }
        FileModes& allRead() { m_mode = S_IRUSR | S_IRGRP | S_IROTH; return *this; }
        FileModes& privateMode() { m_mode = S_IRUSR | S_IWUSR; return *this; }
        // get the mode value
        mode_t getModes() const { return m_mode; }
    private:
        mode_t m_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // Default: 0644
    };

    //USE_AIO.          AIO+epoll
    //USE_IO_URING.     IO_URING

#ifdef USE_AIO

    class File
    {
    public:
        File(Runtime& runtime);
        File(Runtime& runtime, GHandle handle);
        std::expected<void, CommonError> open(const std::string& path, OpenFlags flags, FileModes modes);
        std::expected<void, CommonError> aioInit(int max_events);
        void preRead(char* buffer, size_t size, LL offset, void* data = nullptr);
        //设置O_APPEND后忽略 offset 参数,现代文件系统允许稀疏文件 offset 大于文件大小也可形成文件空洞，不计入实际占用
        //保证 result 生命周期在下一次 commiy 之后
        void preWrite(char* buffer, size_t size, LL offset, void* data = nullptr);
        //从第一个 Bytes 开始填充
        void preReadV(std::vector<iovec>& vec, LL offset, void* data = nullptr);

        void preWriteV(std::vector<iovec>& vec, LL offset, void* data = nullptr);

        std::expected<uint64_t, CommonError> commit();

        AsyncResult<std::expected<std::vector<io_event>, CommonError>> getEvent(uint64_t& expect_events);

        void clearIocbs();

        AsyncResult<std::expected<void, CommonError>> close();
        ~File();
    private:
        GHandle m_handle;
        GHandle m_event_handle;
        io_context_t m_io_ctx = {0};
        std::vector<iocb> m_iocbs;
        EventScheduler* m_scheduler = nullptr;
    };
#else
    class File
    { 
    public:
        File(Runtime& runtime);
        File(Runtime& runtime, GHandle handle);

        File(const File& other) = delete;
        File(File&& other);
        File& operator=(const File& other) = delete;
        File& operator=(File&& other);
        ~File();
        HandleOption option();
        std::expected<void, CommonError> open(const std::string& path, OpenFlags flags, FileModes modes);
        AsyncResult<std::expected<Bytes, CommonError>> read(char* result, size_t length);
        std::expected<void, CommonError> seek(size_t offset);
        AsyncResult<std::expected<Bytes, CommonError>> write(Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        std::expected<void, CommonError> remove();
        GHandle getHandle() const;
    private:
        GHandle m_handle;
        std::string m_path;
        EventScheduler* m_scheduler = nullptr;
    };
#endif
}


#endif