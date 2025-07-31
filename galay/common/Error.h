#ifndef __GALAY_ERROR_H__
#define __GALAY_ERROR_H__

#include <cinttypes>
#include <string>
#include <memory>

namespace galay::error
{

    enum ErrorCode
    {
        Error_NoError,
        Error_SocketError,
        Error_BindError,
        Error_ListenError,
        Error_AcceptError,
        Error_ConnectError,
        Error_RecvError,
        Error_RecvfromError,
        Error_SendError,
        Error_SendtoError,
        Error_SendfileError,
        Error_CloseError,
        Error_DisConnectError,
        Error_SSLNewError,
        Error_SSLSetFdError,
        Error_SSLHandshakeError,
        Error_SSLShuntdownError,
        Error_SSLAcceptError,
        Error_SSLConnectError,
        Error_SSLCloseError,
        Error_FileReadError,
        Error_FileWriteError,
        Error_AddEventError,
        Error_ModEventError,
        Error_DelEventError,
        Error_GetSockNameError,
        Error_GetPeerNameError,
        Error_SetSockOptError,
        Error_SetBlockError,
        Error_SetNoBlockError,
        Error_InetNtopError,
        Error_EpollCreateError,
        Error_EventWriteError,
        Error_KqueueCreateError,
        Error_OpenError,
        Error_LinuxAioSetupError,
        Error_LinuxAioSubmitError,
        Error_SystemErrorEnd
    };

    

    class Error
    {
    public:
        using ptr = std::shared_ptr<Error>;
        Error(uint64_t code) : m_code(code) {}
        virtual std::string message() const = 0;
        virtual uint64_t code() const { return m_code; }
    protected:
        uint64_t m_code = 0;
    };

    class SystemError : public Error
    {
    public:
        SystemError(uint32_t galay_code, uint32_t system_code);
        std::string message() const override;
    private:
        uint64_t makeErrorCode(uint32_t galay_code, uint32_t system_code);
    };

}

#endif