#include "FileEvent.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "File.h"

namespace galay::details
{
#ifdef USE_AIO
    FileCloseEvent::FileCloseEvent(GHandle event_handle, EventScheduler* scheduler, GHandle handle)
        : FileEvent<ValueWrapper<bool>>(event_handle, scheduler), m_handle(handle)
    {
    }

    bool FileCloseEvent::ready()
    {   
        using namespace error;
        if(::close(m_handle.fd) < 0) {
            Error::ptr error = std::make_shared<SystemError>(ErrorCode::CallCloseError, errno);
            makeValue(this->m_result, error);
        }
        if(::close(m_ehandle.fd) < 0) {
            Error::ptr error = std::make_shared<SystemError>(ErrorCode::CallCloseError, errno);
            makeValue(this->m_result, error);
        }
        m_scheduler->removeEvent(this, nullptr);
        return true;
    }

    FileCommitEvent::FileCommitEvent(GHandle event_handle, EventScheduler* scheduler, io_context_t context, std::vector<iocb>&& iocbs)
        : FileEvent<ValueWrapper<bool>>(event_handle, scheduler), m_unfinished_cb(iocbs.size()), m_context(context), m_iocbs(std::move(iocbs))
    {
    }

    bool FileCommitEvent::ready()
    {
        using namespace error;
        Error::ptr error = nullptr;
        size_t nums = m_iocbs.size();
        std::vector<iocb*> iocb_ptrs(nums, nullptr);
        for (size_t i = 0; i < nums; i++) {
            iocb_ptrs[i] = &m_iocbs[i];
        }
        if(io_submit(m_context, nums, iocb_ptrs.data()) == -1) {
            error = std::make_shared<SystemError>(CallAioSubmitError, errno);
            makeValue(m_result, false, error);
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
                        for(size_t i = 0; i < result->size(); ++i) {
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
        m_unfinished_cb -= finish_event;
        if(m_unfinished_cb == 0) {
            m_scheduler->removeEvent(this, nullptr);
            m_waker.wakeUp();
        } else {
            m_scheduler->activeEvent(this, nullptr);
        }
    
    } 
#endif
}
