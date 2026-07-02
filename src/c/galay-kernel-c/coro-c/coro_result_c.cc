#include "coro_result_c.h"

extern "C" {

const char* galay_coro_ioresult_string(C_IOResultCode code)
{
    switch (code) {
    case C_IOResultOk:
        return "ok";
    case C_IOResultEof:
        return "eof";
    case C_IOResultTimeout:
        return "timeout";
    case C_IOResultCancelled:
        return "cancelled";
    case C_IOResultInvalid:
        return "invalid";
    case C_IOResultError:
        return "error";
    default:
        return "unknown";
    }
}

galay_status_t galay_coro_ioresult_to_status(C_IOResultCode code)
{
    switch (code) {
    case C_IOResultOk:
        return GALAY_OK;
    case C_IOResultEof:
        return GALAY_EOF;
    case C_IOResultTimeout:
        return GALAY_TIMEOUT;
    case C_IOResultCancelled:
        return GALAY_CANCELLED;
    case C_IOResultInvalid:
        return GALAY_INVALID_ARGUMENT;
    case C_IOResultError:
        return GALAY_IO_ERROR;
    default:
        return GALAY_INTERNAL_ERROR;
    }
}

} // extern "C"
