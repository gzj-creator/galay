#include <galay/c/galay-mcp/mcp.h>

#include <stdio.h>

int main(void)
{
    galay_mcp_message_t* message = NULL;
    const char* data = NULL;
    size_t data_len = 0;

    if (galay_mcp_message_create(&message) != GALAY_OK) {
        return 1;
    }
    if (galay_mcp_build_ping_request(message, 1) != GALAY_OK ||
        galay_mcp_message_data(message, &data, &data_len) != GALAY_OK) {
        galay_mcp_message_destroy(message);
        return 1;
    }

    fwrite(data, 1, data_len, stdout);
    galay_mcp_message_destroy(message);
    return 0;
}
