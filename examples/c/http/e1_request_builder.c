#include <galay/c/galay-http/http.h>

#include <stdio.h>

int main(void)
{
    galay_http_request_t* request = NULL;
    const char* data = NULL;
    size_t data_len = 0;

    if (galay_http_request_create(&request) != GALAY_OK) {
        return 1;
    }
    if (galay_http_request_set_method(request, GALAY_HTTP_METHOD_POST) != GALAY_OK ||
        galay_http_request_set_uri(request, "/submit") != GALAY_OK ||
        galay_http_request_add_header(request, "Host", "example.test") != GALAY_OK ||
        galay_http_request_set_body(request, "hello", 5) != GALAY_OK ||
        galay_http_request_serialize(request, &data, &data_len) != GALAY_OK) {
        galay_http_request_destroy(request);
        return 1;
    }

    fwrite(data, 1, data_len, stdout);
    galay_http_request_destroy(request);
    return 0;
}
