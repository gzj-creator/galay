#include <galay/c/galay-http2/http2.h>

int main(void)
{
    return GALAY_HTTP2_FRAME_HEADER_LENGTH == 9 ? 0 : 1;
}
