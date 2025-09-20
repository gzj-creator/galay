#include "File.h"
#ifdef USE_AIO
    #include <sys/eventfd.h>
#endif
namespace galay 
{
#ifdef USE_AIO

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
            return std::unexpected<CommonError>({ErrorCode::CallAioSetupError, static_cast<uint32_t>(-ret)});
        }
        return {};
    }

    void File::preRead(char* buffer, size_t size, LL offset, void* data)
    {
        m_iocbs.push_back(iocb{});
        io_prep_pread(&m_iocbs.back(), m_handle.fd, buffer, size, offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = data;
    }

    void File::preWrite(char* buffer, size_t size, LL offset, void* data)
    {
        m_iocbs.push_back(iocb{});
        io_prep_pwrite(&m_iocbs.back(), m_handle.fd, buffer, size, offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = data;
    }

    void File::preReadV(std::vector<iovec>& vec, LL offset, void* data)
    {
        m_iocbs.push_back(iocb{});
        io_prep_preadv(&m_iocbs.back(), m_handle.fd, vec.data(), vec.size(), offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = data;
    }

    void File::preWriteV(std::vector<iovec>& vec, LL offset, void* data)
    {
        m_iocbs.push_back(iocb{});
        io_prep_pwritev(&m_iocbs.back(), m_handle.fd, vec.data(), static_cast<int>(vec.size()), offset);
        io_set_eventfd(&m_iocbs.back(), m_event_handle.fd);
        m_iocbs.back().data = data;
    }

    std::expected<uint64_t, CommonError> File::commit()
    {
        using namespace error;
        size_t nums = m_iocbs.size();
        std::vector<iocb*> iocb_ptrs(nums, nullptr);
        for (size_t i = 0; i < nums; i++) {
            iocb_ptrs[i] = &m_iocbs[i];
        }
        if(io_submit(m_io_ctx, nums, iocb_ptrs.data()) == -1) {
            return std::unexpected(CommonError{CallAioSubmitError, static_cast<uint32_t>(errno)});
        }
        return nums;
    }

    AsyncResult<std::expected<std::vector<io_event>, CommonError>> File::getEvent(uint64_t& expect_events)
    {
        return {std::make_shared<details::AioGetEvent>(m_event_handle, m_scheduler, m_io_ctx, expect_events)};
    }

    void File::clearIocbs()
    {
        m_iocbs.clear();
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
    }

    File::File(Runtime& runtime, GHandle handle)
    {
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    File::File(File&& other)
        : m_handle(other.m_handle), m_scheduler(other.m_scheduler)
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
            other.m_handle = GHandle::invalid();
            other.m_scheduler = nullptr;
        }
        return *this;
    }

    File::~File()
    {
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

    AsyncResult<std::expected<Bytes, CommonError>> File::read(char* buffer, size_t length)
    {
        return {std::make_shared<details::FileReadEvent>(m_handle, m_scheduler, buffer, length)};
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

    std::expected<void, CommonError> File::remove()
    {
        using namespace error;
        if (::remove(m_path.c_str()) == -1) {
            return std::unexpected(CommonError(ErrorCode::CallRemoveError, static_cast<uint32_t>(errno)));
        } 
        return {};
    }

    GHandle File::getHandle() const
    {
        return m_handle;
    }
#endif
}