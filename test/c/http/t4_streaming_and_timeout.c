#include <galay/c/galay-http-c/http_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>

#include <stdio.h>
#include <string.h>

typedef enum ScenarioKind {
    SCENARIO_STREAMING_RESPONSE = 1,
    SCENARIO_TIMEOUT_RESPONSE = 2,
    SCENARIO_MALFORMED_RESPONSE = 3,
    SCENARIO_CLOSED_RESPONSE = 4,
    SCENARIO_OVERSIZED_RESPONSE = 5
} ScenarioKind;

typedef struct ScenarioState {
    ScenarioKind kind;
    galay_http_server_t* server;
    galay_http_client_t* client;
    C_Host endpoint;
    C_IOResult server_result;
    C_IOResult client_result;
    int client_checked;
} ScenarioState;

static int expect_status(galay_status_t actual, galay_status_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "status mismatch: got %d expected %d\n", (int)actual, (int)expected);
        return 1;
    }
    return 0;
}

static int expect_io(C_IOResult actual, C_IOResultCode expected)
{
    if (actual.code != expected) {
        fprintf(stderr, "io mismatch: got %d expected %d errno=%d bytes=%zu\n",
                (int)actual.code, (int)expected, actual.sys_errno, actual.bytes);
        return 1;
    }
    return 0;
}

static int assign_loopback(C_Host* host)
{
    if (host == NULL) {
        return 1;
    }
    host->type = C_IPTypeIPV4;
    int written = snprintf(host->address, sizeof(host->address), "%s", "127.0.0.1");
    if (written <= 0 || (size_t)written >= sizeof(host->address)) {
        return 1;
    }
    host->port = 0;
    return 0;
}

static int string_equals(const char* value, size_t value_len, const char* expected)
{
    return value != NULL && value_len == strlen(expected) &&
           strncmp(value, expected, value_len) == 0;
}

static void server_entry(void* arg)
{
    ScenarioState* state = (ScenarioState*)arg;
    galay_http_session_t* session = NULL;
    galay_http_request_t* request = NULL;
    C_IOResult result =
        galay_http_server_accept(state->server, &session, NULL, 2000);
    if (result.code != C_IOResultOk) {
        state->server_result = result;
        return;
    }

    if (state->kind == SCENARIO_TIMEOUT_RESPONSE) {
        result = galay_http_session_recv_request(session, &request, 4096, 4096, 2000);
        if (request != NULL) {
            galay_http_request_destroy(request);
        }
        if (result.code != C_IOResultOk) {
            state->server_result = result;
        } else {
            char byte = 0;
            state->server_result = galay_http_session_recv_bytes(session, &byte, sizeof(byte), 25);
        }
        result = galay_http_session_close(session, 2000);
        if (result.code != C_IOResultOk && state->server_result.code == C_IOResultOk) {
            state->server_result = result;
        }
        if (galay_http_session_destroy(session) != GALAY_OK &&
            state->server_result.code == C_IOResultOk) {
            state->server_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        }
        return;
    }

    if (state->kind == SCENARIO_CLOSED_RESPONSE) {
        state->server_result = galay_http_session_close(session, 2000);
        if (galay_http_session_destroy(session) != GALAY_OK &&
            state->server_result.code == C_IOResultOk) {
            state->server_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        }
        return;
    }

    if (state->kind == SCENARIO_STREAMING_RESPONSE) {
        const char first[] = "HTTP/1.1 200 OK\r\ncontent-length: 6\r\n\r\nabc";
        const char second[] = "def";
        result = galay_http_session_recv_request(session, &request, 4096, 4096, 2000);
        if (request != NULL) {
            galay_http_request_destroy(request);
        }
        if (result.code == C_IOResultOk) {
            result = galay_http_session_send_bytes(session, first, sizeof(first) - 1, 2000);
        }
        if (result.code == C_IOResultOk) {
            C_IOResult yielded = galay_coro_yield();
            if (yielded.code != C_IOResultOk) {
                result = yielded;
            }
        }
        if (result.code == C_IOResultOk) {
            result = galay_http_session_send_bytes(session, second, sizeof(second) - 1, 2000);
        }
        state->server_result = result;
    } else if (state->kind == SCENARIO_MALFORMED_RESPONSE) {
        const char bad[] = "not an http response\r\n\r\n";
        state->server_result =
            galay_http_session_send_bytes(session, bad, sizeof(bad) - 1, 2000);
    } else {
        const char large[] = "HTTP/1.1 200 OK\r\ncontent-length: 6\r\n\r\nabcdef";
        state->server_result =
            galay_http_session_send_bytes(session, large, sizeof(large) - 1, 2000);
    }

    result = galay_http_session_close(session, 2000);
    if (result.code != C_IOResultOk && state->server_result.code == C_IOResultOk) {
        state->server_result = result;
    }
    if (galay_http_session_destroy(session) != GALAY_OK &&
        state->server_result.code == C_IOResultOk) {
        state->server_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
    }
}

static void client_entry(void* arg)
{
    ScenarioState* state = (ScenarioState*)arg;
    galay_http_request_t* request = NULL;
    galay_http_response_t* response = NULL;
    const char* body = NULL;
    size_t body_len = 0;

    if (galay_http_request_create(&request) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        return;
    }
    if (galay_http_request_set_method_path(request, GALAY_HTTP_METHOD_GET, "/stream") != GALAY_OK ||
        galay_http_request_set_body(request, NULL, 0) != GALAY_OK) {
        state->client_result = (C_IOResult){C_IOResultError, 0, 0, 0, NULL};
        galay_http_request_destroy(request);
        return;
    }

    state->client_result = galay_http_client_connect(state->client, &state->endpoint, 2000);
    if (state->client_result.code == C_IOResultOk &&
        state->kind != SCENARIO_CLOSED_RESPONSE) {
        state->client_result = galay_http_client_send_request(state->client, request, 2000);
    }
    if (state->client_result.code == C_IOResultOk) {
        size_t max_body = state->kind == SCENARIO_OVERSIZED_RESPONSE ? 4 : 4096;
        int64_t timeout = state->kind == SCENARIO_TIMEOUT_RESPONSE ? 5 : 2000;
        state->client_result =
            galay_http_client_recv_response(state->client, &response, 4096, max_body, timeout);
    }

    if (state->kind == SCENARIO_STREAMING_RESPONSE &&
        state->client_result.code == C_IOResultOk &&
        galay_http_response_body(response, &body, &body_len) == GALAY_OK &&
        string_equals(body, body_len, "abcdef")) {
        state->client_checked = 1;
    } else if (state->kind == SCENARIO_TIMEOUT_RESPONSE &&
               state->client_result.code == C_IOResultTimeout) {
        state->client_checked = 1;
    } else if (state->kind == SCENARIO_MALFORMED_RESPONSE &&
               state->client_result.code == C_IOResultError) {
        state->client_checked = 1;
    } else if (state->kind == SCENARIO_CLOSED_RESPONSE &&
               state->client_result.code == C_IOResultEof) {
        state->client_checked = 1;
    } else if (state->kind == SCENARIO_OVERSIZED_RESPONSE &&
               state->client_result.code == C_IOResultError) {
        state->client_checked = 1;
    }

    if (response != NULL) {
        galay_http_response_destroy(response);
    }
    galay_http_request_destroy(request);
}

static int cleanup(galay_kernel_runtime_t* runtime,
                   galay_coro_task_t* server_task,
                   galay_coro_task_t* client_task,
                   ScenarioState* state,
                   int exit_code)
{
    if (server_task->task != NULL) {
        C_IOResult destroy_result = galay_coro_destroy(server_task);
        if (destroy_result.code != C_IOResultOk && exit_code == 0) {
            exit_code = 80;
        }
    }
    if (client_task->task != NULL) {
        C_IOResult destroy_result = galay_coro_destroy(client_task);
        if (destroy_result.code != C_IOResultOk && exit_code == 0) {
            exit_code = 81;
        }
    }
    if (state->client != NULL) {
        C_IOResult close_result = galay_http_client_close(state->client, 2000);
        if (close_result.code != C_IOResultOk &&
            close_result.code != C_IOResultInvalid &&
            exit_code == 0) {
            exit_code = 82;
        }
        if (galay_http_client_destroy(state->client) != GALAY_OK && exit_code == 0) {
            exit_code = 83;
        }
        state->client = NULL;
    }
    if (state->server != NULL) {
        C_IOResult stop_result = galay_http_server_stop(state->server, 2000);
        if (stop_result.code != C_IOResultOk &&
            stop_result.code != C_IOResultInvalid &&
            exit_code == 0) {
            exit_code = 84;
        }
        if (galay_http_server_destroy(state->server) != GALAY_OK && exit_code == 0) {
            exit_code = 85;
        }
        state->server = NULL;
    }
    if (runtime->runtime != NULL) {
        if (galay_kernel_runtime_stop(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 86;
        }
        if (galay_kernel_runtime_destroy(runtime) != C_RuntimeSuccess && exit_code == 0) {
            exit_code = 87;
        }
    }
    return exit_code;
}

static int run_scenario(ScenarioKind kind)
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 0;

    galay_kernel_runtime_t runtime = {0};
    galay_coro_task_t server_task = {0};
    galay_coro_task_t client_task = {0};
    ScenarioState state = {0};
    state.kind = kind;

    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess) {
        return 1;
    }
    if (galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        return cleanup(&runtime, &server_task, &client_task, &state, 2);
    }
    if (expect_status(galay_http_server_create(&state.server), GALAY_OK) ||
        expect_status(galay_http_client_create(&state.client), GALAY_OK)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 3);
    }
    if (assign_loopback(&state.endpoint) != 0 ||
        expect_status(galay_http_server_bind(state.server, &state.endpoint), GALAY_OK) ||
        expect_status(galay_http_server_listen(state.server, 16), GALAY_OK) ||
        expect_status(galay_http_server_local_endpoint(state.server, &state.endpoint), GALAY_OK)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 4);
    }

    if (expect_io(galay_coro_spawn(&runtime, server_entry, &state, NULL, &server_task),
                  C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 5);
    }
    if (expect_io(galay_coro_spawn(&runtime, client_entry, &state, NULL, &client_task),
                  C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 6);
    }
    if (expect_io(galay_coro_join(&client_task, 3000), C_IOResultOk) ||
        expect_io(galay_coro_join(&server_task, 3000), C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 7);
    }
    if (state.client_checked != 1) {
        fprintf(stderr, "scenario %d client check failed with code %d\n",
                (int)kind, (int)state.client_result.code);
        return cleanup(&runtime, &server_task, &client_task, &state, 8);
    }
    if (kind == SCENARIO_TIMEOUT_RESPONSE) {
        if (state.server_result.code != C_IOResultTimeout) {
            fprintf(stderr, "timeout server expected timeout, got %d\n",
                    (int)state.server_result.code);
            return cleanup(&runtime, &server_task, &client_task, &state, 9);
        }
    } else if (expect_io(state.server_result, C_IOResultOk)) {
        return cleanup(&runtime, &server_task, &client_task, &state, 10);
    }

    return cleanup(&runtime, &server_task, &client_task, &state, 0);
}

int main(void)
{
    int result = run_scenario(SCENARIO_STREAMING_RESPONSE);
    if (result != 0) {
        return 10 + result;
    }
    result = run_scenario(SCENARIO_TIMEOUT_RESPONSE);
    if (result != 0) {
        return 20 + result;
    }
    result = run_scenario(SCENARIO_MALFORMED_RESPONSE);
    if (result != 0) {
        return 30 + result;
    }
    result = run_scenario(SCENARIO_CLOSED_RESPONSE);
    if (result != 0) {
        return 40 + result;
    }
    result = run_scenario(SCENARIO_OVERSIZED_RESPONSE);
    if (result != 0) {
        return 50 + result;
    }
    return 0;
}
