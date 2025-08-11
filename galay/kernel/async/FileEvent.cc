#include "FileEvent.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "File.h"

namespace galay::details
{

    FileCloseEvent::FileCloseEvent(FileStatusContext &context)
        : FileEvent<ValueWrapper<bool>>(context)
    {
    }

    void FileCloseEvent::handleEvent()
    {
    }


    bool FileCloseEvent::ready()
    {
        return false;
    }


    bool FileCloseEvent::suspend(Waker waker)
    {
        using namespace error;
        Error::ptr error = nullptr;
        bool success = true;
        if(m_context.m_handle.flags[0] == 1) m_context.m_scheduler->delEvent(this, nullptr);
        if(::close(m_context.m_handle.fd))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallCloseError, errno);
            success = false;
        } else {
            m_context.m_handle = GHandle::invalid();
        }
        makeValue(m_result, std::move(success), error);
        return false;
    }


    FileCommitEvent::FileCommitEvent(FileStatusContext &context)
        : FileEvent<ValueWrapper<bool>>(context)
    {
    }

    bool FileCommitEvent::ready()
    {
        using namespace error;
        Error::ptr error = nullptr;
        std::vector<iocb*> iocbs(m_context.m_unfinished, nullptr);
        for (int i = 0; i < m_context.m_unfinished; i++) {
            iocbs[i] = &m_context.m_iocbs[i];
        }
        if(io_submit(m_context.m_io_ctx, m_context.m_unfinished, iocbs.data()) == -1) {
            error = std::make_shared<SystemError>(CallAioSubmitError, errno);
            makeValue(m_result, false, error);
            return true;
        }
        return false;
    }

    bool FileCommitEvent::suspend(Waker waker)
    {
        using namespace error;
        if(m_context.m_handle.flags[0] == 0)
        {
            if(!m_context.m_scheduler->addEvent(this, nullptr)) {
                SystemError::ptr error = std::make_shared<SystemError>(CallAddEventError, errno);
                makeValue(m_result, false, error);
                return false;
            }
        } else {
            if(!m_context.m_scheduler->modEvent(this, nullptr)) {
                SystemError::ptr error = std::make_shared<SystemError>(CallModEventError, errno);
                makeValue(m_result, false, error);
                return false;
            }
        }
        return FileEvent::suspend(waker);
    }

    void FileCommitEvent::handleEvent()
    {
        uint64_t finish_event = 0;
        int ret = read(m_context.m_event_handle.fd, &finish_event, sizeof(finish_event));
        std::vector<io_event> events(finish_event);
        ret = io_getevents(m_context.m_io_ctx, 1, events.size(), events.data(), nullptr);
        while (ret -- > 0)
        {
            auto& event = events[ret];
            if(event.data) {
                if(event.obj->aio_lio_opcode == IO_CMD_PREAD) {
                    Bytes* result = static_cast<Bytes*>(event.data);
                    if(event.res > 0) {
                        BytesVisitor visitor(*result);
                        visitor.size() = event.res;
                    }
                } else if(event.obj->aio_lio_opcode == IO_CMD_PWRITE) {
                    int *result = static_cast<int*>(event.data);
                    *result = event.res;
                } 
                else if(event.obj->aio_lio_opcode == IO_CMD_PREADV) {
                    std::vector<Bytes>* result = static_cast<std::vector<Bytes>*>(event.data);
                    if(event.res > 0) { 
                        unsigned long remain = event.res;
                        for(int i = 0; i < result->size(); ++i) {
                            if(remain > result->at(i).capacity()) {
                                BytesVisitor visitor(result->at(i));
                                visitor.size() = result->at(i).capacity();
                                remain -= result->at(i).size();
                            } else {
                                BytesVisitor visitor(result->at(i));
                                visitor.size() = remain;
                                remain -= result->at(i).size();
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
        m_context.m_unfinished -= finish_event;
        if(m_context.m_unfinished == 0) {
            m_context.m_scheduler->delEvent(this, nullptr);
            m_waker.wakeUp();
        } else {
            m_context.m_scheduler->modEvent(this, nullptr);
        }
    
    } 
}
