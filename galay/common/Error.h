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
        CallShuntdownError,
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
        ConcurrentError,
        AioEventsAllCompleteError,
        ErrorEnd
    };

    class CommonError
    {
    public:
        static bool contains(uint64_t error, ErrorCode code);
        CommonError(uint32_t galay_code, uint32_t system_code);
        uint64_t code() const;
        std::string message() const;
        void reset();
    private:
        uint64_t makeErrorCode(uint32_t galay_code, uint32_t system_code);
    private:
        uint64_t m_code;
    };

}

#endif