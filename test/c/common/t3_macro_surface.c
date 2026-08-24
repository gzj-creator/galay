#include <stddef.h>

#include <galay/c/galay-common-c/common/macro.h>
#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-http2-c/macro.h>
#include <galay/c/galay-mongo-c/macro.h>
#include <galay/c/galay-rpc-c/macro.h>
#include <galay/c/galay-tracing-c/macro.h>
#include <galay/c/galay-utils-c/utils.h>
#include <galay/c/galay-kernel-c/common-c/macro.h>

#if GALAY_C_API != 1 || GALAY_C_VERSION_MAJOR != 4u
#error "common C ABI macros are not exposed by the centralized macro header"
#endif

int main(void)
{
    return GALAY_HTTP2_FRAME_HEADER_LENGTH == 9u &&
           GALAY_MONGO_MAX_KEY_LENGTH == 255u &&
           GALAY_MONGO_MAX_STRING_LENGTH == 4096u &&
           GALAY_RPC_HEADER_SIZE == 16u &&
           GALAY_TRACING_TRACE_ID_HEX_LENGTH == 32u &&
           GALAY_TRACING_SPAN_ID_HEX_LENGTH == 16u &&
           GALAY_TRACING_TRACEPARENT_LENGTH == 55u &&
           C_HOST_ADDRESS_MAX_LENGTH == 46 &&
           C_RUNTIME_SCHEDULER_COUNT_AUTO == (size_t)-1
        ? 0
        : 1;
}
