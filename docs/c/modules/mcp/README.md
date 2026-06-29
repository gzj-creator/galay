# MCP C Runtime

The MCP C API exposes opaque client/server handles over the direct C coroutine result model. Public async-style calls return `C_IOResult`; recoverable API errors are reported through the result code and `value` carries the mapped `galay_status_t` when applicable.

## Ownership

- `galay_mcp_client_config_t`, `galay_mcp_client_t`, `galay_mcp_server_t`, and `galay_mcp_message_t` are opaque owning handles.
- Destroy configs with `galay_mcp_client_config_destroy`, clients with `galay_mcp_client_destroy`, servers with `galay_mcp_server_destroy`, and returned messages with `galay_mcp_message_destroy`.
- `galay_mcp_message_data` returns a borrowed pointer valid until the message is mutated or destroyed.
- Handler callbacks receive borrowed argument/name/URI slices and an owned output message object they must fill with `galay_mcp_message_set_json`.

## Message Helpers

`galay_mcp_message_create`, `galay_mcp_message_set_json`, and
`galay_mcp_message_data` manage raw JSON message buffers. Request/notification
builders are `galay_mcp_build_request`, `galay_mcp_build_notification`,
`galay_mcp_build_initialized_notification`, and
`galay_mcp_build_empty_result_response`.

`galay_mcp_parse_request` and `galay_mcp_parse_response` return parsed handles;
release them with `galay_mcp_parsed_request_destroy` or
`galay_mcp_parsed_response_destroy`. Accessors return borrowed JSON or string
slices owned by the parsed handle.

## Client Runtime

- Stdio loopback uses `galay_mcp_client_connect_stdio_loopback(client, server, timeout_ms)` and dispatches requests directly to the registered C server handlers.
- HTTP mode uses `galay_mcp_http_config_create("http://127.0.0.1:PORT/mcp", ...)`, `galay_mcp_client_connect_async`, and one TCP HTTP POST per MCP request.
- HTTP bearer-token auth is configured with `galay_mcp_http_server_require_bearer_token` and `galay_mcp_http_config_set_bearer_token`; mismatches are returned as MCP/JSON-RPC failures through `C_IOResult`.
- Supported client operations are `initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, `prompts/list`, `prompts/get`, and `disconnect`.
- Operation results that carry JSON return an owned `galay_mcp_message_t`.

## Server Runtime

- `galay_mcp_stdio_server_create` creates an in-process stdio-style loopback server.
- `galay_mcp_http_server_create`, `galay_mcp_http_server_start`, and `galay_mcp_http_server_serve_once` provide a local HTTP loopback server over the C kernel TCP socket runtime.
- Register tools, resources, and prompts with explicit callback pointers and userdata.
- Tool/resource/prompt callback failures are converted to JSON-RPC error responses; no C++ exception boundary is introduced.

## Thread And Coroutine Notes

- Stdio loopback calls are immediate and do not require a running C coroutine.
- HTTP client requests and `galay_mcp_http_server_serve_once` call kernel TCP coroutine APIs and must run inside a task created with `galay_coro_spawn`.
- `galay_mcp_http_server_start`, endpoint lookup, and stop/destroy are synchronous lifecycle operations.
- Dynamic handler registration is intended before serving requests; concurrent mutation of registrations while serving is not supported.

## Verification

The module tests cover JSON-RPC helpers, stdio initialize/ping/listTools/callTool/disconnect, HTTP loopback for the same client flow, and server handler registration for tools/resources/prompts.
