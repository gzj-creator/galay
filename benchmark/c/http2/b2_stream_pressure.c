#include <galay/c/galay-http2-c/http2.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct PressureBench {
    size_t streams;
    galay_http2_server_t* server;
    uint16_t port;
    C_IOResult server_result;
    C_IOResult client_result;
    size_t resets_observed;
} PressureBench;

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int make_headers(galay_http2_headers_t** out)
{
    if (galay_http2_headers_create(out) != GALAY_OK) {
        return 1;
    }
    if (galay_http2_headers_add(*out, ":method", "GET") != GALAY_OK ||
        galay_http2_headers_add(*out, ":scheme", "http") != GALAY_OK ||
        galay_http2_headers_add(*out, ":authority", "127.0.0.1") != GALAY_OK ||
        galay_http2_headers_add(*out, ":path", "/pressure") != GALAY_OK) {
        galay_http2_headers_destroy(*out);
        *out = 0;
        return 1;
    }
    return 0;
}

static void pressure_server_entry(void* arg)
{
    PressureBench* bench = (PressureBench*)arg;
    galay_http2_conn_t* conn = 0;
    bench->server_result = galay_http2_server_accept(bench->server, &conn, 5000);
    if (bench->server_result.code != C_IOResultOk) {
        return;
    }
    for (size_t i = 0; i < bench->streams; ++i) {
        galay_http2_stream_t* stream = 0;
        galay_http2_headers_t* headers = 0;
        char data[1] = {0};
        size_t read_len = 0;
        galay_bool_t end_stream = GALAY_FALSE;
        bench->server_result = galay_http2_conn_accept_stream(conn, &stream, 5000);
        if (bench->server_result.code == C_IOResultOk) {
            bench->server_result = galay_http2_stream_read_headers(stream, &headers, 0);
        }
        if (bench->server_result.code == C_IOResultOk) {
            C_IOResult reset =
                galay_http2_stream_read_data(stream, data, sizeof(data), &read_len, &end_stream, 5000);
            if (reset.code == C_IOResultError && reset.value == GALAY_HTTP2_ERROR_STREAM_RESET) {
                ++bench->resets_observed;
                bench->server_result = (C_IOResult){C_IOResultOk, 0, 0, 0, 0};
            } else {
                bench->server_result = reset;
            }
        }
        if (headers != 0) {
            galay_http2_headers_destroy(headers);
        }
        if (stream != 0 &&
            galay_http2_stream_destroy(stream) != GALAY_OK &&
            bench->server_result.code == C_IOResultOk) {
            bench->server_result = (C_IOResult){C_IOResultError, 0, 0,
                                                GALAY_HTTP2_ERROR_INTERNAL, 0};
        }
        if (bench->server_result.code != C_IOResultOk) {
            break;
        }
    }
    if (conn != 0 && galay_http2_conn_destroy(conn) != GALAY_OK &&
        bench->server_result.code == C_IOResultOk) {
        bench->server_result = (C_IOResult){C_IOResultError, 0, 0,
                                            GALAY_HTTP2_ERROR_INTERNAL, 0};
    }
}

static void pressure_client_entry(void* arg)
{
    PressureBench* bench = (PressureBench*)arg;
    galay_http2_client_t* client = 0;
    galay_http2_config_t config = galay_http2_config_default();
    config.host = "127.0.0.1";
    config.port = bench->port;
    config.max_concurrent_streams = (uint32_t)(bench->streams + 8);

    if (galay_http2_client_create(&config, &client) != GALAY_OK) {
        bench->client_result = (C_IOResult){C_IOResultError, 0, 0,
                                            GALAY_HTTP2_ERROR_INTERNAL, 0};
        goto cleanup;
    }
    bench->client_result = galay_http2_client_connect(client, 5000);
    for (size_t i = 0; i < bench->streams && bench->client_result.code == C_IOResultOk; ++i) {
        galay_http2_headers_t* headers = 0;
        galay_http2_stream_t* stream = 0;
        if (make_headers(&headers) != 0) {
            bench->client_result = (C_IOResult){C_IOResultError, 0, 0,
                                                GALAY_HTTP2_ERROR_INTERNAL, 0};
            break;
        }
        bench->client_result =
            galay_http2_client_open_stream(client, headers, GALAY_FALSE, &stream, 5000);
        if (bench->client_result.code == C_IOResultOk) {
            bench->client_result =
                galay_http2_stream_reset(stream, GALAY_HTTP2_ERROR_CANCEL, 5000);
        }
        if (headers != 0) {
            galay_http2_headers_destroy(headers);
        }
        if (stream != 0 &&
            galay_http2_stream_destroy(stream) != GALAY_OK &&
            bench->client_result.code == C_IOResultOk) {
            bench->client_result = (C_IOResult){C_IOResultError, 0, 0,
                                                GALAY_HTTP2_ERROR_INTERNAL, 0};
        }
    }

cleanup:
    if (client != 0) {
        galay_http2_client_destroy(client);
    }
}

int main(int argc, char** argv)
{
    size_t streams = 200;
    if (argc == 3 && strcmp(argv[1], "-n") == 0) {
        streams = (size_t)strtoull(argv[2], 0, 10);
    }
    if (streams == 0) {
        return 1;
    }

    C_RuntimeConfig runtime_config = galay_kernel_runtime_config_default();
    runtime_config.io_scheduler_count = 1;
    runtime_config.compute_scheduler_count = 0;
    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    PressureBench bench = {.streams = streams};
    int exit_code = 0;

    galay_http2_config_t server_config = galay_http2_config_default();
    server_config.host = "127.0.0.1";
    server_config.max_concurrent_streams = (uint32_t)(streams + 8);

    const int64_t start_ns = now_ns();
    if (galay_kernel_runtime_create(&runtime_config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess ||
        galay_http2_server_create(&server_config, &bench.server) != GALAY_OK ||
        galay_http2_server_listen(bench.server, &bench.port).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, pressure_server_entry, &bench, 0, &server_task).code != C_IOResultOk ||
        galay_coro_spawn(&runtime, pressure_client_entry, &bench, 0, &client_task).code != C_IOResultOk ||
        galay_coro_join(&server_task, 10000).code != C_IOResultOk ||
        galay_coro_join(&client_task, 10000).code != C_IOResultOk ||
        bench.server_result.code != C_IOResultOk ||
        bench.client_result.code != C_IOResultOk ||
        bench.resets_observed != streams) {
        exit_code = 2;
    }
    const int64_t elapsed_ns = now_ns() - start_ns;
    const double seconds = elapsed_ns > 0 ? (double)elapsed_ns / 1000000000.0 : 0.0;
    if (exit_code == 0 &&
        printf("http2_stream_pressure streams=%zu resets=%zu seconds=%.6f qps=%.2f errors=0\n",
               streams,
               bench.resets_observed,
               seconds,
               seconds > 0.0 ? (double)streams / seconds : 0.0) < 0) {
        exit_code = 3;
    }

    if (server_task.task != 0 && galay_coro_destroy(&server_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 4;
    }
    if (client_task.task != 0 && galay_coro_destroy(&client_task).code != C_IOResultOk &&
        exit_code == 0) {
        exit_code = 5;
    }
    if (bench.server != 0) {
        if (galay_http2_server_stop(bench.server, 1000).code != C_IOResultOk && exit_code == 0) {
            exit_code = 6;
        }
        galay_http2_server_destroy(bench.server);
    }
    if (runtime.runtime != 0) {
        if (galay_kernel_runtime_stop(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 7;
        }
        if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 8;
        }
    }
    return exit_code;
}
