#ifndef __GALAY_ERROR_H__
#define __GALAY_ERROR_H__

#include <cinttypes>
#include <string>
#include <memory>

namespace galay::error
{

    enum ErrorCode
    {
        NoError,
        DisConnectError,
        CallSocketError,
        CallBindError,
        CallListenError,
        CallAcceptError,
        CallConnectError,
        CallRecvError,
        CallRecvfromError,
        CallSendError,
        CallSendtoError,
        CallSendfileError,
        CallCloseError,
        CallSSLNewError,
        CallSSLSetFdError,
        CallSSLHandshakeError,
        CallSSLShuntdownError,
        CallSSLAcceptError,
        CallSSLConnectError,
        CallSSLCloseError,
        CallFileReadError,
        CallFileWriteError,
        CallLSeekError,
        CallRemoveError,
        CallActiveEventError,
        CallRemoveEventError,
        CallGetSockNameError,
        CallGetPeerNameError,
        CallSetSockOptError,
        CallSetBlockError,
        CallSetNoBlockError,
        CallInetNtopError,
        CallEpollCreateError,
        CallEventWriteError,
        CallKqueueCreateError,
        CallOpenError,
        CallAioSetupError,
        CallAioSubmitError,
        NotInitializedError,
        AsyncTimeoutError,
        NotifyButSourceNotReadyError,
        FileReadEmptyError,
        FileWriteEmptyError,
        SystemErrorEnd
    };

    

    class Error
    {
    public:
        using ptr = std::shared_ptr<Error>;
        Error(uint64_t code) : m_code(code) {}
        virtual std::string message() const = 0;
        virtual uint64_t code() const { return m_code; }
        virtual ~Error() = default;
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