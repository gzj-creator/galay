# galay C ABI 参考

galay 的 C ABI 用纯 C 封装底层 C++ 协程框架。每个模块位于 `src/c/galay-<name>-c/`，通过 **opaque handle + 显式结果码** 暴露稳定接口，**不跨边界抛 C++ 异常**。C 侧文档在 `docs/c/modules/<name>/`，示例在 `examples/c/<module>/*.c`。

## 通用约定

### 头文件前缀

```c
#include <galay/c/galay-<name>-c/...>
// 例:
#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-http-c/http_c.h>
```

### CMake link 目标

`galay::c-common`、`galay::c-kernel`、`galay::c-bridge`、`galay::c-http`、`galay::c-http2`、`galay::c-ws`、`galay::c-redis`、`galay::c-mysql`、`galay::c-mongo`、`galay::c-etcd`、`galay::c-rpc`、`galay::c-mcp`、`galay::c-ssl`、`galay::c-tracing`、`galay::c-utils`。

多数模块 `PUBLIC` 依赖 `galay::c-common galay::c-kernel`（mongo/ssl/ws 额外链接对应 C++ 库 `galay::mongo` / `galay::ssl` / `galay::ws`）。构建需 `-DGALAY_BUILD_C_API=ON`（默认 ON）。

### 错误处理约定

- **同步/生命周期操作** 返回 `galay_status_t`（`galay_c_error.h`），`GALAY_OK = 0` 成功，其余分类错误码：
  `GALAY_INVALID_ARGUMENT(1)`、`GALAY_NOT_FOUND(2)`、`GALAY_OUT_OF_MEMORY(3)`、`GALAY_PROTOCOL_ERROR(4)`、`GALAY_UNSUPPORTED(5)`、`GALAY_IO_ERROR(6)`、`GALAY_INTERNAL_ERROR(7)`、`GALAY_EOF(8)`、`GALAY_TIMEOUT(9)`、`GALAY_CANCELLED(10)`。
- **协程 I/O 操作** 返回 `C_IOResult` 值类型（`coro_result_c.h`）：
  ```c
  typedef struct C_IOResult {
      C_IOResultCode code;   // 必须首先检查
      int    sys_errno;      // 归一化系统 errno；无错误为 0
      size_t bytes;          // 读写/处理字节或条目数
      int64_t value;         // 附加整数（如 fd）
      void*  ptr;            // 附加指针（所有权见各 API 注释）
  } C_IOResult;
  typedef enum C_IOResultCode {
      C_IOResultOk, C_IOResultEof, C_IOResultTimeout,
      C_IOResultCancelled, C_IOResultInvalid, C_IOResultError
  } C_IOResultCode;
  ```
  必须先判断 `code == C_IOResultOk` 再读取其它字段。
- **runtime** 独立枚举 `C_RuntimeResultCode`：`C_RuntimeSuccess`、`C_RuntimeParameterInvalid`、`C_RuntimeMemoryAllocFailed`、`C_RuntimeStartFailed`。
- **`*_get_error(...)` 字符串函数**：每个公开错误枚举都有转字符串入口，返回库拥有的静态只读串（不可释放），未知值返回 `"unknown"`：`galay_status_string(status)`、`galay_http_get_error(status)`、`galay_kernel_runtime_get_error(C_RuntimeResultCode)`、`galay_coro_ioresult_string(C_IOResultCode)`，以及 `galay_coro_ioresult_to_status(code)` 把 I/O 码归一化为 `galay_status_t`。
- **create/destroy 所有权**：handle 由 ABI 内 `*_create` 分配、`*_destroy` 释放；调用方不得解引用或自行 free 内部 `void*`。getter/serialize 返回的指针是**借用**内部存储，在对象 mutation、重新 parse/serialize 或 destroy 后失效。
- **协程/runtime 驱动**：所有 I/O API 必须在 `galay_coro_spawn` 创建的 C 协程内调用（挂起协程栈而非阻塞 OS 线程）；入口 `galay_coro_entry_fn` 必须正常返回、不得抛 C++ 异常。典型模式：`runtime_create → runtime_start → coro_spawn(entry) → coro_join → coro_destroy → runtime_stop → runtime_destroy`。

### 命名规范

函数 `galay_<module>_<action>(...)`；类型 `galay_<module>_<type>_t`；跨模块共享值类型带 `C_` 前缀（`C_Host`、`C_IOResult`、`C_RuntimeConfig`）；`galay_bool_t` 只有 `GALAY_FALSE=0` / `GALAY_TRUE=1`。

## 核心 C 类型与入口

**Runtime**（`core-c/runtime_c.h`）— `C_RUNTIME_SCHEDULER_COUNT_AUTO == (size_t)-1`：
```c
typedef struct C_RuntimeConfig { size_t io_scheduler_count; size_t compute_scheduler_count; } C_RuntimeConfig;
typedef struct galay_kernel_runtime { void* runtime; } galay_kernel_runtime_t;

C_RuntimeConfig     galay_kernel_runtime_config_default(void);
C_RuntimeResultCode galay_kernel_runtime_create(const C_RuntimeConfig* config, galay_kernel_runtime_t* c_runtime);
C_RuntimeResultCode galay_kernel_runtime_start(galay_kernel_runtime_t* c_runtime);
C_RuntimeResultCode galay_kernel_runtime_stop(galay_kernel_runtime_t* c_runtime);
bool                galay_kernel_runtime_is_running(const galay_kernel_runtime_t* c_runtime);
C_RuntimeResultCode galay_kernel_runtime_destroy(galay_kernel_runtime_t* c_runtime);
```

**Coro Task**（`coro-c/coro_task_c.h`）：
```c
typedef void (*galay_coro_entry_fn)(void* arg);
typedef struct C_CoroOptions { size_t stack_size; } C_CoroOptions;      // 0 = 默认栈
typedef struct galay_coro_task { void* task; } galay_coro_task_t;

C_CoroOptions galay_coro_options_default(void);
C_IOResult galay_coro_spawn(galay_kernel_runtime_t* runtime, galay_coro_entry_fn entry,
                            void* arg, const C_CoroOptions* options, galay_coro_task_t* out_task);
C_IOResult galay_coro_yield(void);
C_IOResult galay_coro_current(galay_coro_task_t* out_task);
C_IOResult galay_coro_join(galay_coro_task_t* task, int64_t timeout_ms);  // <0 无限, 0 轮询, >0 毫秒
C_IOResult galay_coro_cancel(galay_coro_task_t* task);
C_IOResult galay_coro_destroy(galay_coro_task_t* task);                   // 成功后 task->task = NULL
```
> `join` 不能在协程内或任意 scheduler 线程调用（返回 `C_IOResultInvalid`）；销毁 runtime 前必须先 destroy 所有协程任务。

**C_Host**（`common-c/host.h`）— `C_HOST_ADDRESS_MAX_LENGTH == 46`：
```c
typedef enum C_IPType { C_IPTypeIPV4 = 0, C_IPTypeIPV6 = 1 } C_IPType;
typedef struct C_Host { C_IPType type; char address[46]; uint16_t port; } C_Host;
```

**HTTP handle 与关键签名**（`galay-http-c/http_c.h`）— opaque：`galay_http_server_t`、`galay_http_client_t`、`galay_http_request_t`、`galay_http_response_t`、`galay_http_headers_t`、`galay_http_session_t`；枚举 `galay_http_method_t`（GET/POST）、`galay_http_status_code_t`（200/204/400/404/500）：
```c
// server（生命周期在协程外做，serve_one 在协程内）
galay_status_t galay_http_server_create(galay_http_server_t** out);
galay_status_t galay_http_server_bind(galay_http_server_t* server, const C_Host* endpoint);
galay_status_t galay_http_server_listen(galay_http_server_t* server, int backlog);
galay_status_t galay_http_server_local_endpoint(const galay_http_server_t* server, C_Host* endpoint);
galay_status_t galay_http_server_add_route(galay_http_server_t* server, galay_http_method_t method,
                                           const char* path, galay_http_route_callback_t callback, void* user_data);
C_IOResult     galay_http_server_serve_one(galay_http_server_t* server, int64_t timeout_ms); // accept+route+resp+close
C_IOResult     galay_http_server_stop(galay_http_server_t* server, int64_t timeout_ms);
galay_status_t galay_http_server_destroy(galay_http_server_t* server);
typedef galay_status_t (*galay_http_route_callback_t)(const galay_http_request_t* request,
                                                      galay_http_response_t* response, void* user_data);
// client（I/O 均在协程内）
galay_status_t galay_http_client_create(galay_http_client_t** out);
C_IOResult     galay_http_client_connect(galay_http_client_t* client, const C_Host* endpoint, int64_t timeout_ms);
C_IOResult     galay_http_client_send_request(galay_http_client_t* client, const galay_http_request_t* request, int64_t timeout_ms);
C_IOResult     galay_http_client_recv_response(galay_http_client_t* client, galay_http_response_t** out_response,
                                               size_t max_header_len, size_t max_body_len, int64_t timeout_ms);
C_IOResult     galay_http_client_close(galay_http_client_t* client, int64_t timeout_ms);
galay_status_t galay_http_client_destroy(galay_http_client_t* client);
// request / response（setter 复制入内，getter 借用）
galay_status_t galay_http_request_create(galay_http_request_t** out);
galay_status_t galay_http_request_set_method_path(galay_http_request_t* request, galay_http_method_t method, const char* path);
galay_status_t galay_http_request_add_header(galay_http_request_t* request, const char* name, const char* value);
galay_status_t galay_http_request_set_body(galay_http_request_t* request, const char* body, size_t body_len);
galay_status_t galay_http_request_path(const galay_http_request_t* request, const char** path, size_t* path_len);
galay_status_t galay_http_request_body(const galay_http_request_t* request, const char** body, size_t* body_len);
void           galay_http_request_destroy(galay_http_request_t* request);
galay_status_t galay_http_response_create(galay_http_response_t** out);
galay_status_t galay_http_response_set_status(galay_http_response_t* response, galay_http_status_code_t status);
galay_status_t galay_http_response_set_body(galay_http_response_t* response, const char* body, size_t body_len);
galay_status_t galay_http_response_body(const galay_http_response_t* response, const char** body, size_t* body_len);
void           galay_http_response_destroy(galay_http_response_t* response);
```
> 另有 `galay_http_server_accept` + `galay_http_session_*`（send/recv request/response、send/recv bytes、close、destroy）用于手动 session 级流式控制；`headers_create/add/find/remove/destroy` 独立管理 header 集合；`request/response_serialize/parse` 做无 socket 编解码。

## 每模块入口速查

| 模块 | 头文件前缀 | link 目标 | 关键入口类型 |
|------|-----------|-----------|-------------|
| common | `galay/c/galay-common-c/common/` | `galay::c-common` | `galay_status_t`, `galay_bool_t` |
| kernel | `galay/c/galay-kernel-c/` | `galay::c-kernel` | `galay_kernel_runtime_t`, `galay_coro_task_t`, `C_Host`, `C_IOResult` |
| bridge | `galay/c/galay-bridge-c/coro-c/` | `galay::c-bridge` | coro TCP/UDP/file/mutex/waiter bridge |
| http | `galay/c/galay-http-c/http_c.h` | `galay::c-http` | `galay_http_server_t`, `galay_http_client_t`, `galay_http_request_t`, `galay_http_response_t` |
| http2 | `galay/c/galay-http2-c/http2_c.h` | `galay::c-http2` | `galay_http2_client_t`, `galay_http2_conn_t`, `galay_http2_frame_t` |
| ws | `galay/c/galay-ws-c/ws_c.h` | `galay::c-ws` | `galay_ws_client_t`, `galay_ws_connection_t`, `galay_ws_frame_t` |
| redis | `galay/c/galay-redis-c/redis_c.h` | `galay::c-redis` | `galay_redis_client_t`, `galay_redis_cluster_t`, `galay_redis_command_builder_t` |
| mysql | `galay/c/galay-mysql-c/mysql_c.h` | `galay::c-mysql` | `galay_mysql_client_t`, `galay_mysql_config_t`, `galay_mysql_field_view_t` |
| mongo | `galay/c/galay-mongo-c/mongo_c.h` | `galay::c-mongo` | `galay_mongo_client_t`, `galay_mongo_document_t`, `galay_mongo_uri_t` |
| etcd | `galay/c/galay-etcd-c/etcd_c.h` | `galay::c-etcd` | `galay_etcd_client_t`, `galay_etcd_config_builder_t`, `galay_etcd_get_result_t` |
| rpc | `galay/c/galay-rpc-c/rpc_c.h` | `galay::c-rpc` | `galay_rpc_client_t`, `galay_rpc_call_options_t`, `galay_rpc_cancellation_source_t` |
| mcp | `galay/c/galay-mcp-c/mcp_c.h` | `galay::c-mcp` | `galay_mcp_client_t`, `galay_mcp_message_t`, `galay_mcp_parsed_request_t` |
| ssl | `galay/c/galay-ssl-c/ssl_c.h` | `galay::c-ssl` | `galay_ssl_context_t`, `galay_ssl_socket_t` |
| tracing | `galay/c/galay-tracing-c/tracing_c.h` | `galay::c-tracing` | `galay_tracing_provider_t`, `galay_tracing_logger_t` |
| utils | — | `galay::c-utils` | （工具类） |

## 最小示例

从 `examples/c/http/e1_async_client.c` / `e2_async_server.c` 提炼的 client+server 单请求闭环：

```c
#include <galay/c/galay-http-c/http_c.h>
#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <string.h>

typedef struct { galay_http_server_t* srv; galay_http_client_t* cli; C_Host ep;
                 C_IOResult sr, cr; } State;

static galay_status_t route(const galay_http_request_t* req, galay_http_response_t* resp, void* ud) {
    (void)req; (void)ud;
    if (galay_http_response_set_status(resp, GALAY_HTTP_STATUS_OK) != GALAY_OK) return GALAY_INTERNAL_ERROR;
    return galay_http_response_set_body(resp, "hello", 5);          // getter 借用, setter 复制
}
static void server_entry(void* a) { State* s = a; s->sr = galay_http_server_serve_one(s->srv, 2000); }
static void client_entry(void* a) {
    State* s = a; galay_http_request_t* rq = NULL; galay_http_response_t* rp = NULL;
    if (galay_http_request_create(&rq) != GALAY_OK) { s->cr.code = C_IOResultError; return; }
    galay_http_request_set_method_path(rq, GALAY_HTTP_METHOD_GET, "/hello");
    s->cr = galay_http_client_connect(s->cli, &s->ep, 2000);
    if (s->cr.code == C_IOResultOk) s->cr = galay_http_client_send_request(s->cli, rq, 2000);
    if (s->cr.code == C_IOResultOk) s->cr = galay_http_client_recv_response(s->cli, &rp, 4096, 4096, 2000);
    if (rp) galay_http_response_destroy(rp);
    galay_http_request_destroy(rq);
}
int main(void) {
    C_RuntimeConfig cfg = galay_kernel_runtime_config_default();
    cfg.io_scheduler_count = 1; cfg.compute_scheduler_count = 0;
    galay_kernel_runtime_t rt = {0}; galay_coro_task_t st = {0}, ct = {0}; State s = {0};

    if (galay_kernel_runtime_create(&cfg, &rt) != C_RuntimeSuccess) return 1;
    galay_kernel_runtime_start(&rt);
    galay_http_server_create(&s.srv); galay_http_client_create(&s.cli);
    s.ep.type = C_IPTypeIPV4; strcpy(s.ep.address, "127.0.0.1"); s.ep.port = 0;
    galay_http_server_bind(s.srv, &s.ep); galay_http_server_listen(s.srv, 16);
    galay_http_server_local_endpoint(s.srv, &s.ep);                // 读回系统分配端口
    galay_http_server_add_route(s.srv, GALAY_HTTP_METHOD_GET, "/hello", route, &s);

    galay_coro_spawn(&rt, server_entry, &s, NULL, &st);
    galay_coro_spawn(&rt, client_entry, &s, NULL, &ct);
    galay_coro_join(&st, 3000); galay_coro_join(&ct, 3000);        // 均须 code == C_IOResultOk

    galay_coro_destroy(&st); galay_coro_destroy(&ct);              // 先销毁协程, 再关连接
    galay_http_client_close(s.cli, 2000);  galay_http_client_destroy(s.cli);
    galay_http_server_stop(s.srv, 2000);   galay_http_server_destroy(s.srv);
    galay_kernel_runtime_stop(&rt);        galay_kernel_runtime_destroy(&rt);
    return (s.sr.code == C_IOResultOk && s.cr.code == C_IOResultOk) ? 0 : 2;
}
```
