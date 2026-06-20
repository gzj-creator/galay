#include <galay/c/galay-http/http.h>

#include <stdio.h>
#include <string.h>

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "require failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define REQUIRE_STATUS(expr, expected) \
    do { \
        galay_status_t got_status = (expr); \
        if (got_status != (expected)) { \
            fprintf(stderr, "status failed: %s:%d: got %d expected %d\n", \
                    __FILE__, __LINE__, (int)got_status, (int)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_header_add_find_remove(void)
{
    galay_http_headers_t* headers = NULL;
    const char* value = NULL;
    size_t value_len = 0;

    REQUIRE_STATUS(galay_http_headers_create(&headers), GALAY_OK);
    REQUIRE_STATUS(galay_http_headers_add(headers, "Accept", "text/plain"), GALAY_OK);
    REQUIRE_STATUS(galay_http_headers_add(headers, "Accept", "application/json"), GALAY_OK);
    REQUIRE_STATUS(galay_http_headers_find(headers, "accept", &value, &value_len), GALAY_OK);
    REQUIRE_TRUE(value != NULL);
    REQUIRE_TRUE(value_len == strlen("text/plain, application/json"));
    REQUIRE_TRUE(strncmp(value, "text/plain, application/json", value_len) == 0);

    REQUIRE_STATUS(galay_http_headers_remove(headers, "ACCEPT"), GALAY_OK);
    REQUIRE_STATUS(galay_http_headers_find(headers, "accept", &value, &value_len), GALAY_NOT_FOUND);

    galay_http_headers_destroy(headers);
    return 0;
}

static int test_request_builder_and_empty_body_parse(void)
{
    const char raw[] =
        "GET /empty HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    galay_http_request_t* request = NULL;
    size_t consumed = 0;
    const char* body = NULL;
    size_t body_len = 99;
    const char* host = NULL;
    size_t host_len = 0;

    REQUIRE_STATUS(galay_http_request_create(&request), GALAY_OK);
    REQUIRE_STATUS(galay_http_request_parse(request, raw, sizeof(raw) - 1,
                                            1024, 1024, &consumed), GALAY_OK);
    REQUIRE_TRUE(consumed == sizeof(raw) - 1);
    REQUIRE_TRUE(galay_http_request_is_complete(request) == GALAY_TRUE);
    REQUIRE_STATUS(galay_http_request_body(request, &body, &body_len), GALAY_OK);
    REQUIRE_TRUE(body != NULL);
    REQUIRE_TRUE(body_len == 0);
    REQUIRE_STATUS(galay_http_request_find_header(request, "HOST", &host, &host_len), GALAY_OK);
    REQUIRE_TRUE(host_len == strlen("example.test"));
    REQUIRE_TRUE(strncmp(host, "example.test", host_len) == 0);

    galay_http_request_destroy(request);
    return 0;
}

static int test_malformed_request_rejected(void)
{
    const char raw[] = "GET / HTTP/9.9\r\n\r\n";
    galay_http_request_t* request = NULL;
    size_t consumed = 0;

    REQUIRE_STATUS(galay_http_request_create(&request), GALAY_OK);
    REQUIRE_STATUS(galay_http_request_parse(request, raw, sizeof(raw) - 1,
                                            1024, 1024, &consumed),
                   GALAY_PROTOCOL_ERROR);

    galay_http_request_destroy(request);
    return 0;
}

static int test_oversized_header_rejected(void)
{
    const char raw[] =
        "GET / HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "\r\n";
    galay_http_request_t* request = NULL;
    size_t consumed = 0;

    REQUIRE_STATUS(galay_http_request_create(&request), GALAY_OK);
    REQUIRE_STATUS(galay_http_request_parse(request, raw, sizeof(raw) - 1,
                                            strlen("GET / HTTP/1.1\r\n") - 1,
                                            1024, &consumed),
                   GALAY_PROTOCOL_ERROR);

    galay_http_request_destroy(request);
    return 0;
}

static int test_response_builder_serializes_empty_body(void)
{
    galay_http_response_t* response = NULL;
    const char* data = NULL;
    size_t data_len = 0;

    REQUIRE_STATUS(galay_http_response_create(&response), GALAY_OK);
    REQUIRE_STATUS(galay_http_response_set_status(response, GALAY_HTTP_STATUS_NO_CONTENT), GALAY_OK);
    REQUIRE_STATUS(galay_http_response_set_body(response, NULL, 0), GALAY_OK);
    REQUIRE_STATUS(galay_http_response_serialize(response, &data, &data_len), GALAY_OK);
    REQUIRE_TRUE(data != NULL);
    REQUIRE_TRUE(data_len > 0);
    REQUIRE_TRUE(strstr(data, "HTTP/1.1 204 No Content\r\n") == data);
    REQUIRE_TRUE(strstr(data, "content-length: 0\r\n") != NULL);

    galay_http_response_destroy(response);
    return 0;
}

int main(void)
{
    if (test_header_add_find_remove() != 0) {
        return 1;
    }
    if (test_request_builder_and_empty_body_parse() != 0) {
        return 1;
    }
    if (test_malformed_request_rejected() != 0) {
        return 1;
    }
    if (test_oversized_header_rejected() != 0) {
        return 1;
    }
    if (test_response_builder_serializes_empty_body() != 0) {
        return 1;
    }
    return 0;
}
