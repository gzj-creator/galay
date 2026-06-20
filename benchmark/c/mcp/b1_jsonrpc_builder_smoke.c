#include <galay/c/galay-mcp/mcp.h>

#include <stdio.h>

int main(void)
{
    galay_mcp_message_t* message = NULL;
    if (galay_mcp_message_create(&message) != GALAY_OK) {
        return 1;
    }

    for (int i = 0; i < 10000; ++i) {
        if (galay_mcp_build_request(message, i, "tools/list", "{}") != GALAY_OK) {
            galay_mcp_message_destroy(message);
            return 1;
        }
    }

    galay_mcp_message_destroy(message);
    puts("c mcp jsonrpc builder smoke: 10000 iterations");
    return 0;
}
