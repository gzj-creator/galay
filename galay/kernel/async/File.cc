#include "File.h"
#include <sys/eventfd.h>

namespace galay 
{
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
        m_context.m_event_handle.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
        m_context.m_scheduler = runtime.eventScheduler();
    }

    File::File(Runtime &runtime, GHandle handle)
    {
        m_context.m_event_handle.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
        m_context.m_handle = handle;
        m_context.m_scheduler = runtime.eventScheduler();
    }

    ValueWrapper<bool> File::open(const std::string &path, OpenFlags flags, FileModes modes)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        SystemError::ptr error = nullptr;
        const int fd = ::open(path.c_str(), flags.getFlags(), modes.getModes());
        if( fd < 0 ) {
            error = std::make_shared<SystemError>(ErrorCode::CallOpenError, errno);
            makeValue(wrapper, false, error);
            return wrapper;
        }
        m_context.m_handle.fd = fd;
        HandleOption option(GHandle{fd});
        option.handleNonBlock();
        makeValue(wrapper, true, error);
        return wrapper;
    }

    ValueWrapper<bool> File::aioInit(int max_events)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        int ret = io_setup(max_events, &m_context.m_io_ctx);
        if(ret) {
            error = std::make_shared<SystemError>(ErrorCode::CallAioSetupError, errno);
            makeValue(wrapper, false, error);
            return wrapper;
        }
        m_context.m_iocbs.resize(max_events);
        makeValue(wrapper, true, error);  // 修复：成功时也需要设置返回值
        return wrapper;
    }

    void File::preRead(Bytes& bytes, LL offset)
    {
        m_context.m_unfinished++;
        io_prep_pread(&m_context.m_iocbs[m_context.m_iocb_index++], m_context.m_handle.fd, bytes.data(), bytes.capacity(), offset);
        io_set_eventfd(&m_context.m_iocbs[m_context.m_iocb_index - 1], m_context.m_event_handle.fd);
        m_context.m_iocbs[m_context.m_iocb_index - 1].data = &bytes;
    }

    void File::preWrite(Bytes& bytes, int &result, LL offset)
    {
        m_context.m_unfinished++;
        io_prep_pwrite(&m_context.m_iocbs[m_context.m_iocb_index++], m_context.m_handle.fd, bytes.data(), bytes.size(), offset);
        io_set_eventfd(&m_context.m_iocbs[m_context.m_iocb_index - 1], m_context.m_event_handle.fd);
        m_context.m_iocbs[m_context.m_iocb_index - 1].data = &result;
    }

    void File::preReadV(std::vector<Bytes>& bytes_v, IOVecResult &temp, LL offset)
    {
        m_context.m_unfinished++;
        IOVecResultVisitor visitor(temp);
        visitor.iovecs().resize(bytes_v.size());
        for(int i = 0; i < bytes_v.size(); ++i){
            visitor.iovecs()[i].iov_base = bytes_v[i].data();
            visitor.iovecs()[i].iov_len = bytes_v[i].capacity();
        }
        io_prep_preadv(&m_context.m_iocbs[m_context.m_iocb_index++], m_context.m_handle.fd, visitor.iovecs().data(), visitor.iovecs().size(), offset);
        io_set_eventfd(&m_context.m_iocbs[m_context.m_iocb_index - 1], m_context.m_event_handle.fd);
        m_context.m_iocbs[m_context.m_iocb_index - 1].data = &bytes_v;
    }

    void File::preWriteV(std::vector<Bytes*> &bytes_v, IOVecResult &result, LL offset)
    {
        m_context.m_unfinished++;
        IOVecResultVisitor visitor(result);
        visitor.iovecs().resize(bytes_v.size());
        for(int i = 0; i < bytes_v.size(); ++i){
            visitor.iovecs()[i].iov_base = bytes_v[i]->data();
            visitor.iovecs()[i].iov_len = bytes_v[i]->size();
        }
        io_prep_pwritev(&m_context.m_iocbs[m_context.m_iocb_index++], m_context.m_handle.fd, visitor.iovecs().data(), visitor.iovecs().size(), offset);
        io_set_eventfd(&m_context.m_iocbs[m_context.m_iocb_index - 1], m_context.m_event_handle.fd);
        m_context.m_iocbs[m_context.m_iocb_index - 1].data = &result;
    }

    AsyncResult<ValueWrapper<bool>> File::commit()
    {
        m_context.m_iocb_index = 0;
        return {std::make_shared<details::FileCommitEvent>(m_context)};
    }

    AsyncResult<ValueWrapper<bool>> File::close()
    {
        return {std::make_shared<details::FileCloseEvent>(m_context)};
    }

    File::~File()
    {
        io_destroy(m_context.m_io_ctx);
        ::close(m_context.m_event_handle.fd);
    }
}