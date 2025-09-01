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
    class IOVecResultVisitor;

    class IOVecResult
    {
        friend class IOVecResultVisitor;
    public:
        unsigned long result();
    private:
        unsigned long m_result;
        std::vector<iovec> m_iovecs;
    };

    class IOVecResultVisitor
    {
    public:
        IOVecResultVisitor(IOVecResult& result);
        unsigned long& result();
        std::vector<iovec>& iovecs();
    private:
        IOVecResult& m_result;
    };

    class File
    {
    public:
        File(Runtime& runtime);
        File(Runtime& runtime, GHandle handle);
        ValueWrapper<bool> open(const std::string& path, OpenFlags flags, FileModes modes);
        ValueWrapper<bool> aioInit(int max_events);
        void preRead(StringMetaData& bytes, LL offset);
        //设置O_APPEND后忽略 offset 参数,现代文件系统允许稀疏文件 offset 大于文件大小也可形成文件空洞，不计入实际占用
        //保证 result 生命周期在下一次 commiy 之后
        void preWrite(StringMetaData& bytes, int& result, LL offset);
        //从第一个 Bytes 开始填充
        void preReadV(std::vector<StringMetaData>& bytes_v, IOVecResult& result, LL offset);

        void preWriteV(std::vector<StringMetaData>& bytes_v, IOVecResult &result, LL offset);

        AsyncResult<ValueWrapper<bool>> commit();
        AsyncResult<ValueWrapper<bool>> close();
        ~File();
    private:
        GHandle m_handle;
        GHandle m_event_handle;
        io_context_t m_io_ctx;
        std::vector<iocb> m_iocbs;
        EventScheduler* m_scheduler = nullptr;
    };
#else
    class File
    { 
    public:
        File(Runtime& runtime);
        File(Runtime& runtime, GHandle handle);

        File(const File& other);
        File(File&& other);
        File& operator=(const File& other);
        File& operator=(File&& other);
        ~File();

        HandleOption option();
        ValueWrapper<bool> open(const std::string& path, OpenFlags flags, FileModes modes);
        AsyncResult<ValueWrapper<Bytes>> read(size_t length);
        ValueWrapper<bool> seek(size_t offset);
        AsyncResult<ValueWrapper<Bytes>> write(Bytes bytes);
        AsyncResult<ValueWrapper<bool>> close();
        void reallocBuffer(size_t length);
        ValueWrapper<bool> remove();
    private:
        GHandle m_handle;
        std::string m_path;
        StringMetaData m_buffer;
        EventScheduler* m_scheduler = nullptr;
    };
#endif
}


#endif