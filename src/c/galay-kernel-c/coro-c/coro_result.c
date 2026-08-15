#include "coro_result.h"
#include "../../galay-common-c/common/galay_c_error.h"

const char* galay_c_coro_ioresult_string(C_IOResultCode code)
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
    case C_IOResultClosed:
        return "closed";
    }
    return "unknown";
}

const char* galay_c_coro_ioresult_get_error(C_IOResultCode code)
{
    return galay_c_coro_ioresult_string(code);
}

galay_status_t galay_c_coro_ioresult_to_status(C_IOResultCode code)
{
    switch (code) {
    case C_IOResultOk:
        return GALAY_OK;
    case C_IOResultEof:
    case C_IOResultClosed:
        return GALAY_EOF;
    case C_IOResultTimeout:
        return GALAY_TIMEOUT;
    case C_IOResultCancelled:
        return GALAY_CANCELLED;
    case C_IOResultInvalid:
        return GALAY_INVALID_ARGUMENT;
    case C_IOResultError:
        return GALAY_IO_ERROR;
    }
    return GALAY_INTERNAL_ERROR;
}
