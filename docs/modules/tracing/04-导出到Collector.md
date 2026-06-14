# 导出到 Collector

`galay-tracing` 当前支持 OTLP/HTTP JSON trace export。默认 endpoint 是 `http://127.0.0.1:4318/v1/traces`，可以配置 endpoint、timeout、headers、resource attributes 和 instrumentation scope。

## 直接导出

```cpp
#include "galay-tracing/kernel/otlp_http_exporter.h"

galay::tracing::OtlpHttpExporterConfig config{
    .endpoint = "http://collector.internal:4318/v1/traces",
    .timeout = std::chrono::milliseconds(500),
    .headers = {{"authorization", "Bearer token"}},
    .resource_attributes = {
        galay::tracing::spanAttribute("service.name", "order-service"),
        galay::tracing::spanAttribute("deployment.environment", "prod"),
    },
    .scope = {
        .name = "order-handler",
        .version = "1.2.3",
    },
};

galay::tracing::OtlpHttpExporter exporter(config);
```

如果构建时启用了 `GALAY_TRACING_ENABLE_OTLP_HTTP`，默认 transport 使用 `galay-http`。否则需要传入自定义 transport，或者 exporter 会返回失败并带错误信息。

## 自定义 Transport

```cpp
auto transport = [](galay::tracing::OtlpHttpRequest request) {
    // Send request.method/request.endpoint/request.headers/request.body
    // through an existing HTTP stack.
    return galay::tracing::OtlpHttpResponse{.status_code = 200};
};

galay::tracing::OtlpHttpExporter exporter(config, transport);
```

`OtlpHttpRequest::headers` 是借用视图，只在 transport 调用期间有效。`body` 由 request 持有，可以 move 到 HTTP 客户端。

## BatchSpanProcessor

```cpp
auto exporter = std::make_unique<galay::tracing::OtlpHttpExporter>(config);
galay::tracing::BatchSpanProcessor processor(std::move(exporter), {
    .queue_capacity = 4096,
    .max_batch_size = 512,
    .flush_interval = std::chrono::milliseconds(500),
    .schedule_mode = galay::tracing::BatchSpanScheduleMode::kBatchSize,
});

galay::tracing::Span span("operation", context);
span.end();
processor.onEnd(std::move(span));
processor.forceFlush(std::chrono::seconds(2));
```

`schedule_mode` 控制后台批处理线程的唤醒策略：`kTimed` 仅按 `flush_interval` 定时处理，`kOnEnd` 每次 sampled span 入队后唤醒，`kBatchSize` 在队列达到 `max_batch_size` 后唤醒。`forceFlush()` 和 `shutdown()` 始终会立即唤醒后台线程并排空队列。

当前版本没有全局 tracer provider，也没有自动把 `SpanGuard` 结束事件送进 processor。生产接入时需要在边界层或业务封装里显式调用 `processor.onEnd(...)`。

## Collector 兼容性

只要服务接受 OTLP/HTTP JSON 的 `/v1/traces` 请求，就可以作为 collector。常见目标包括 OpenTelemetry Collector、兼容 OTLP/HTTP JSON 的代理服务，或内部 mock collector。

当前不支持：

- OTLP/gRPC。
- OTLP/protobuf HTTP body。
- gzip 压缩。
- exporter retry/backoff。
- logs/metrics export。

OTLP JSON trace span 当前会输出 resource attributes、instrumentation scope、trace/span id、parent span id、name、kind、tracestate、status 和 span attributes。Attributes 使用 OTLP `AnyValue` 形态编码，整型以 JSON string 形式输出到 `intValue`。

## Scheduler Thread 阻塞保护

`makeGalayHttpOtlpTransport()` 是同步 transport：调用 `exportSpans()` 的线程会等待 HTTP 完成。默认配置会拒绝在 `galay-kernel` scheduler thread 上执行同步导出：

```cpp
auto transport = galay::tracing::makeGalayHttpOtlpTransport({
    .io_scheduler_count = 1,
    .reject_on_runtime_thread = true,
});
```

如果业务运行在协程调度器内，应把 span 放入 `BatchSpanProcessor`，让后台线程执行 export，避免阻塞 scheduler。

## Endpoint 示例

```text
http://127.0.0.1:4318/v1/traces
http://otel-collector:4318/v1/traces
http://collector.internal:4318/v1/traces
```

当前 `galay-http` 默认 transport 不处理 HTTPS。需要 HTTPS 时，传入已有 TLS HTTP 客户端实现的 custom transport。

## 示例程序

`examples/include/e3_otlp_exporter.cc` 展示了如何配置 endpoint/headers，并用 custom transport 接收生成好的 OTLP JSON body：

```bash
cmake --build build-dev --target E3-OtlpExporter
./build-dev/bin/E3-OtlpExporter
```
