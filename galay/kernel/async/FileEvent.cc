#include "FileEvent.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "File.h"

namespace galay::details
{
#ifdef USE_AIO
    FileCloseEvent::FileCloseEvent(GHandle event_handle, EventScheduler* scheduler, GHandle handle)
        : FileEvent<std::expected<void, CommonError>>(event_handle, scheduler), m_handle(handle)
    {
    }

    bool FileCloseEvent::ready()
    {   
        using namespace error;
        if(::close(m_handle.fd) < 0) {
            m_result = std::unexpected(CommonError{ErrorCode::CallCloseError, static_cast<uint32_t>(errno)});
        } else {
            m_result = {};
        }
        m_scheduler->removeEvent(this, nullptr);
        return true;
    }

    FileCommitEvent::FileCommitEvent(GHandle event_handle, EventScheduler* scheduler, io_context_t context, std::vector<iocb>&& iocbs)
        : FileEvent<std::expected<void, CommonError>>(event_handle, scheduler), m_unfinished_cb(iocbs.size()), m_context(context), m_iocbs(std::move(iocbs))
    {
    }

    bool FileCommitEvent::ready()
    {
        using namespace error;
        size_t nums = m_iocbs.size();
        std::vector<iocb*> iocb_ptrs(nums, nullptr);
        for (size_t i = 0; i < nums; i++) {
            iocb_ptrs[i] = &m_iocbs[i];
        }
        if(io_submit(m_context, nums, iocb_ptrs.data()) == -1) {
            m_result = std::unexpected(CommonError{CallAioSubmitError, static_cast<uint32_t>(errno)});
            return true;
        }
        return false;
    }

    void FileCommitEvent::handleEvent()
    {
        uint64_t finish_event = 0;
        int ret = read(m_ehandle.fd, &finish_event, sizeof(finish_event));
        std::vector<io_event> events(finish_event);
        ret = io_getevents(m_context, 0, events.size(), events.data(), nullptr);
        while (ret -- > 0)
        {
            auto& event = events[ret];
            if(event.data) {
                if(event.obj->aio_lio_opcode == IO_CMD_PREAD) {
                    StringMetaData* result = static_cast<StringMetaData*>(event.data);
                    if(event.res > 0) {
                        result->size = event.res;
                    }
                } else if(event.obj->aio_lio_opcode == IO_CMD_PWRITE) {
                    int *result = static_cast<int*>(event.data);
                    *result = event.res;
                } 
                else if(event.obj->aio_lio_opcode == IO_CMD_PREADV) {
                    std::vector<StringMetaData>* result = static_cast<std::vector<StringMetaData>*>(event.data);
                    if(event.res > 0) { 
                        unsigned long remain = event.res;
                        for(size_t i = 0; i < result->size(); ++i) {
                            if(remain > result->at(i).capacity) {
                                result->at(i).size = result->at(i).capacity;
                                remain -= result->at(i).size;
                            } else {
                                result->at(i).size = remain;
                                remain -= result->at(i).size;
                                break;
                            }
                        }
                    }
                }
                else if (event.obj->aio_lio_opcode == IO_CMD_PWRITEV) {
                    IOVecResult *result = static_cast<IOVecResult*>(event.data);
                    IOVecResultVisitor visitor(*result);
                    visitor.result() = event.res;
                }
            }
        }
        m_unfinished_cb -= finish_event;
        if(m_unfinished_cb == 0) {
            m_scheduler->removeEvent(this, nullptr);
            m_result = {};
            m_waker.wakeUp();
        } else {
            m_scheduler->activeEvent(this, nullptr);
        }
    
    } 
#else
    FileCloseEvent::FileCloseEvent(GHandle handle, EventScheduler *scheduler)
        : FileEvent<std::expected<void, CommonError>>(handle, scheduler)
    {
    }

    bool FileCloseEvent::ready()
    {   
        using namespace error;
        if(::close(m_handle.fd) < 0) {
            m_result = std::unexpected(CommonError{ErrorCode::CallCloseError, static_cast<uint32_t>(errno)});
        } else {
            m_result = {};
        }
        m_scheduler->removeEvent(this, nullptr);
        return true;
    }

    FileReadEvent::FileReadEvent(GHandle handle, EventScheduler *scheduler, char* buffer, size_t length)
        : FileEvent<std::expected<Bytes, CommonError>>(handle, scheduler), m_length(length), m_buffer(buffer)
    {
    }

    bool FileReadEvent::ready()
    {
        m_ready = readBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> FileReadEvent::resume()
    {
        if(!m_ready) readBytes(true);
        return FileEvent<std::expected<Bytes, CommonError>>::resume();
    }

    bool FileReadEvent::readBytes(bool notify)
    {
        using namespace error;
        Bytes bytes;
        int recvBytes = read(m_handle.fd, m_buffer, m_length);
        LogInfo("{}, {}, {}", recvBytes, m_length, m_buffer == nullptr);
        if (recvBytes > 0) {
            bytes = Bytes::fromCString(m_buffer, recvBytes, m_length);
            m_result = std::move(bytes);
        } else if (recvBytes == 0) {
            m_result = std::unexpected(CommonError(CallFileReadError, static_cast<uint32_t>(errno)));
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallFileReadError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

    FileWriteEvent::FileWriteEvent(GHandle handle, EventScheduler *scheduler, Bytes &&bytes)
        : FileEvent<std::expected<Bytes, CommonError>>(handle, scheduler), m_bytes(std::move(bytes))
    {
    }

    bool FileWriteEvent::ready()
    {
        m_ready = writeBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> FileWriteEvent::resume()
    {
        if(!m_ready) writeBytes(true);
        return FileEvent<std::expected<Bytes, CommonError>>::resume();
    }

    bool FileWriteEvent::writeBytes(bool notify)
    {
        using namespace error;
        int sendBytes = write(m_handle.fd, m_bytes.data(), m_bytes.size());
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else if (sendBytes == 0) {
            m_result = std::unexpected(CommonError(CallFileWriteError, static_cast<uint32_t>(errno)));
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallFileWriteError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

#endif
}
