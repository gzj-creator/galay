#include "Error.h"
#include <string.h>

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
        "call close function error",
        "call ssl_new function error",
        "call ssl_set_sd function error",
        "call ssl_handshake function error",
        "call ssl_shutdown function error",
        "call ssl_accept function error",
        "call ssl_connect function error",
        "call ssl_close function error",
        "call read function error",
        "call write function Error",
        "call addEvent function error",
        "call modEvent function error",
        "call delEvent function error",
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
        "LinuxAioSubmit Error",
    };

    SystemError::SystemError(uint32_t galay_code, uint32_t system_code)
        : Error(makeErrorCode(galay_code, system_code)) 
    {

    }

    std::string SystemError::message() const
    {
        uint32_t galay_code = m_code & 0xffffffff;
        uint32_t system_code = m_code >> 32;
        std::string str = error_string[galay_code];
        return str + ", system error: " + std::to_string(system_code);
    }

    uint64_t SystemError::makeErrorCode(uint32_t galay_code, uint32_t system_code)
    {
        uint64_t ret = system_code;
        ret = ret << 32;
        return ret | galay_code;
    }
    
}