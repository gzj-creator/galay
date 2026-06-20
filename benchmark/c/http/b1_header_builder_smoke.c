#include <galay/c/galay-http/http.h>

#include <stdio.h>

int main(void)
{
    for (int i = 0; i < 10000; ++i) {
        galay_http_headers_t* headers = NULL;
        if (galay_http_headers_create(&headers) != GALAY_OK) {
            return 1;
        }
        if (galay_http_headers_add(headers, "Accept", "text/plain") != GALAY_OK ||
            galay_http_headers_add(headers, "Accept", "application/json") != GALAY_OK) {
            galay_http_headers_destroy(headers);
            return 1;
        }
        galay_http_headers_destroy(headers);
    }
    puts("c http header builder smoke: 10000 iterations");
    return 0;
}
