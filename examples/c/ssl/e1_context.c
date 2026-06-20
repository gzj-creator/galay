#include <galay/c/galay-ssl/ssl.h>

#include <stdio.h>

int main(void)
{
    galay_ssl_context_t* context = NULL;
    galay_status_t status =
        galay_ssl_context_create(GALAY_SSL_METHOD_TLS_CLIENT, &context);
    if (status != GALAY_OK) {
        fprintf(stderr, "create ssl context failed: %s\n", galay_status_string(status));
        return 1;
    }

    status = galay_ssl_context_set_verify_mode(context, GALAY_SSL_VERIFY_PEER);
    if (status != GALAY_OK) {
        fprintf(stderr, "set verify mode failed: %s\n", galay_status_string(status));
        galay_ssl_context_destroy(context);
        return 1;
    }

    galay_ssl_context_destroy(context);
    return 0;
}
