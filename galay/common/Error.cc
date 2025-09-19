#include "Error.h"
#include <string.h>
#include <sstream>

namespace galay::error
{

    const char* error_string[] = {
        "no error",
        "connection is closed by peer",
        "call scoket function error",
        "call bind function error",
        "call listen function error",
        "call accept function error",
        "call connect function error",
        "call recv function error",
        "call recvfrom function error",
        "call send function error",
        "call sendto function error",
        "call sendfile function error",
        "call shuntdown function error",
        "call close function error",
        "call ssl_new function error",
        "call ssl_set_fd function error",
        "call ssl_handshake function error",
        "call ssl_shutdown function error",
        "call ssl_accept function error",
        "call ssl_connect function error",
        "call ssl_close function error",
        "call read function error",
        "call write function error",
        "call lseek function error",
        "call remove function error",
        "call activeEvent function error",
        "call removeEvent function error",
        "call getsockname function error",
        "call getpeername function error",
        "call setsockopt function error",
        "call handleBlock function error",
        "call handleNoBlock function error",
        "call inet_ntop function error",
        "call epoll_create function error",
        "call eventfd_write function Error",
        "call kqueue function error",
        "call open function error",
        "call aio io_setup function error",
        "call aio io_submit function error",
        "not initialized",
        "async function timeout",
        "notify but source not ready",
        "read return zero error",
        "write return zero error",
        "concurrent error",
        "aio events are all completed"
    };

    bool CommonError::contains(uint64_t error, ErrorCode code)
    {
        uint32_t galay_code = error & 0xffffffff;
        return static_cast<uint32_t>(code) == galay_code;
    }

    CommonError::CommonError(uint32_t galay_code, uint32_t system_code)
        : m_code(makeErrorCode(galay_code, system_code))
    {
    }

    uint64_t CommonError::code() const
    {
        return m_code;
    }

    std::string CommonError::message() const
    {
        uint32_t galay_code = m_code & 0xffffffff;
        uint32_t system_code = m_code >> 32;
        std::stringstream str;
        str << error_string[galay_code];
        if(system_code != 0) {
            str << "(sys:" << strerror(system_code) << ")";
        } else {
            str << "(sys:no error)";
        }
        return str.str();
    }

    void CommonError::reset()
    {
        m_code = 0;
    }

    uint64_t CommonError::makeErrorCode(uint32_t galay_code, uint32_t system_code)
    {
        uint64_t ret = system_code;
        ret = ret << 32;
        return ret | galay_code;
    }
    
}