#include "File.h"
#ifdef USE_AIO
    #include <sys/eventfd.h>
#endif
namespace galay 
{
#ifdef USE_AIO
    unsigned long IOVecResult::result()
    {
        return m_result;
    }

    IOVecResultVisitor::IOVecResultVisitor(IOVecResult &result)
        : m_result(result)
    {
    }

    unsigned long& IOVecResultVisitor::result()
    {
        return m_result.m_result;
    }

    std::vector<iovec>& IOVecResultVisitor::iovecs()
    {
        return m_result.m_iovecs;
    }

    File::File(Runtime& runtime)
    {
        m_event_handle.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    File::File(Runtime &runtime, GHandle handle)
    {
        m_event_handle.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    std::expected<void, CommonError> File::open(const std::string &path, OpenFlags flags, FileModes modes)
    {
        using namespace error;
        const int fd = ::open(path.c_str(), flags.getFlags(), modes.getModes());
        if( fd < 0 ) {
            return std::unexpected<CommonError>({ErrorCode::CallOpenError, static_cast<uint32_t>(errno)});
        }
        m_handle.fd = fd;
        GHandle handle;
        handle.fd = fd;
        HandleOption option(handle);
        option.handleNonBlock();
        return {};
    }

    std::expected<void, CommonError> File::aioInit(int max_events)
    {
        using namespace error;
        int ret = io_setup(max_events, &m_io_ctx);
        if(ret) {
            return std::unexpected<CommonError>({ErrorCode::CallAioSetupError, static_cast<uint32_t>(errno)});
        }
        return {};
    }

    void File::preRead(StringMetaData& bytes, LL offset)
    {
        m_iocbs.push_back(iocb{});
        io_prep_pread(&m_iocbs.back(), m_handle.fd, bytes.data, bytes.capacity, offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = &bytes;
    }

    void File::preWrite(StringMetaData& bytes, int &result, LL offset)
    {
        m_iocbs.push_back(iocb{});
        io_prep_pwrite(&m_iocbs.back(), m_handle.fd, bytes.data, bytes.size, offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = &result;
    }

    void File::preReadV(std::vector<StringMetaData>& bytes_v, IOVecResult &temp, LL offset)
    {
        m_iocbs.push_back(iocb{});
        IOVecResultVisitor visitor(temp);
        visitor.iovecs().resize(bytes_v.size());
        for(size_t i = 0; i < bytes_v.size(); ++i){
            visitor.iovecs()[i].iov_base = bytes_v[i].data;
            visitor.iovecs()[i].iov_len = bytes_v[i].capacity;
        }
        io_prep_preadv(&m_iocbs.back(), m_handle.fd, visitor.iovecs().data(), visitor.iovecs().size(), offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = &bytes_v;
    }

    void File::preWriteV(std::vector<StringMetaData> &bytes_v, IOVecResult &result, LL offset)
    {
        m_iocbs.push_back(iocb{});
        IOVecResultVisitor visitor(result);
        visitor.iovecs().resize(bytes_v.size());
        for(size_t i = 0; i < bytes_v.size(); ++i){
            visitor.iovecs()[i].iov_base = bytes_v[i].data;
            visitor.iovecs()[i].iov_len = bytes_v[i].size;
        }
        io_prep_pwritev(&m_iocbs.back(), m_handle.fd, visitor.iovecs().data(), visitor.iovecs().size(), offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = &result;
    }

    AsyncResult<std::expected<void, CommonError>> File::commit()
    {
        return {std::make_shared<details::FileCommitEvent>(m_event_handle, m_scheduler, m_io_ctx, std::move(m_iocbs))};
    }

    AsyncResult<std::expected<void, CommonError>> File::close()
    {
        return {std::make_shared<details::FileCloseEvent>(m_event_handle, m_scheduler, m_handle)};
    }

    File::~File()
    {
        io_destroy(m_io_ctx);
    }
#else
    File::File(Runtime& runtime)
    {
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    File::File(Runtime& runtime, GHandle handle)
    {
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    File::File(File&& other)
        : m_handle(other.m_handle), m_scheduler(other.m_scheduler), m_buffer(std::move(other.m_buffer))
    {
        other.m_handle = GHandle::invalid();
        other.m_scheduler = nullptr;
    }

    File& File::operator=(File&& other)
    {
        if (this != &other)
        {
            m_handle = other.m_handle;
            m_scheduler = other.m_scheduler;
            m_buffer = std::move(other.m_buffer);
            other.m_handle = GHandle::invalid();
            other.m_scheduler = nullptr;
        }
        return *this;
    }

    File::~File()
    {
        freeString(m_buffer);
    }

    HandleOption File::option()
    {
        return HandleOption(m_handle);
    }

    std::expected<void, CommonError> File::open(const std::string& path, OpenFlags flags, FileModes modes)
    {
        using namespace error;
        m_path = path;
        const int fd = ::open(path.c_str(), flags.getFlags(), modes.getModes());
        if( fd < 0 ) {
            return std::unexpected<CommonError>({ErrorCode::CallOpenError, static_cast<uint32_t>(errno)});
        }
        m_handle.fd = fd;
        GHandle handle;
        handle.fd = fd;
        HandleOption option(handle);
        option.handleNonBlock();
        return {};
    }

    AsyncResult<std::expected<Bytes, CommonError>> File::read(size_t length)
    {
        if(m_buffer.capacity < length) {
            reallocString(m_buffer, length);
        }
        clearString(m_buffer);
        return {std::make_shared<details::FileReadEvent>(m_handle, m_scheduler, reinterpret_cast<char*>(m_buffer.data), length)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> File::write(Bytes bytes)
    {
        return {std::make_shared<details::FileWriteEvent>(m_handle, m_scheduler, std::move(bytes))};
    }

    std::expected<void, CommonError> File::seek(size_t offset)
    {
        using namespace error;
        if (::lseek(m_handle.fd, offset, SEEK_SET) == -1) { 
            return std::unexpected(CommonError(ErrorCode::CallLSeekError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    AsyncResult<std::expected<void, CommonError>> File::close()
    {
        return {std::make_shared<details::FileCloseEvent>(m_handle, m_scheduler)};
    }

    void File::reallocReadBuffer(size_t length)
    {
        reallocString(m_buffer, length);
    }

    std::expected<void, CommonError> File::remove()
    {
        using namespace error;
        if (::remove(m_path.c_str()) == -1) {
            return std::unexpected(CommonError(ErrorCode::CallRemoveError, static_cast<uint32_t>(errno)));
        } 
        return {};
    }
#endif
}