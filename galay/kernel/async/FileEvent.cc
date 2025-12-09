#include "FileEvent.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "File.h"

namespace galay::details
{
#ifdef USE_AIO

    bool FileCloseEvent::onReady()
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

    AioGetEvent::AioGetEvent(GHandle event_handle, EventScheduler* scheduler, io_context_t context, uint64_t& expect_events)
        : FileEvent<std::expected<std::vector<io_event>, CommonError>>(event_handle, scheduler), m_context(context), m_expect_events(expect_events)
    {
    }

    bool AioGetEvent::onReady()
    {
        if(m_expect_events == 0) {
            m_result = std::unexpected(CommonError(AioEventsAllCompleteError, 0));
            m_ready = true;
            return m_ready;
        }
        m_ready = getEvent(false);
        return m_ready;
    }

    void AioGetEvent::handleEvent()
    {
        m_waker.wakeUp();
    }

    std::expected<std::vector<io_event>, CommonError> AioGetEvent::onResume()
    {
        if(!m_ready) getEvent(true);
        return FileEvent<std::expected<std::vector<io_event>, CommonError>>::onResume();
    }

    bool AioGetEvent::getEvent(bool notify)
    {
        uint64_t finish_event = 0;
        int ret = read(m_ehandle.fd, &finish_event, sizeof(finish_event));
        if(ret < 0) {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallFileReadError, static_cast<uint32_t>(errno)));
        } else {
            std::vector<io_event> events(finish_event);
            ret = io_getevents(m_context, finish_event, events.size(), events.data(), nullptr);
            m_expect_events -= ret;
            events.resize(ret);
            m_result = std::move(events);
        }
        return true;
    }

#else

    bool FileCloseEvent::onReady()
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

    bool FileReadEvent::onReady()
    {
        m_ready = readBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> FileReadEvent::onResume()
    {
        if(!m_ready) readBytes(true);
        return FileEvent<std::expected<Bytes, CommonError>>::onResume();
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


    bool FileWriteEvent::onReady()
    {
        m_ready = writeBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> FileWriteEvent::onResume()
    {
        if(!m_ready) writeBytes(true);
        return FileEvent<std::expected<Bytes, CommonError>>::onResume();
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
