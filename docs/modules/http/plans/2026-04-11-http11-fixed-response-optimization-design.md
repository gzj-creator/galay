# HTTP/1.1 Fixed Response Optimization Design

**Date:** 2026-04-11

**Goal**

在不扩大回归面的前提下，先优化 `HTTP/1.1` benchmark 固定 `200 OK` keep-alive 响应路径，优先缩小 `B1-HttpServer` 与 Rust 对照的吞吐差距，并同步验证 `B14-HttpsServer` 是否跟随受益。

**Scope**

本轮只覆盖以下路径：

- `benchmark/b1_http.cc`
- `benchmark/b14_https.cc`
- `galay-http/kernel/http/http_writer.h`

本轮不进入：

- `HTTP/2` 或 `h2c`
- 路由、动态 header、动态 body
- `galay-kernel` / `galay-ssl`

**Current Evidence**

- 旧 HTTP profile 已显示 `Http1_1ResponseBuilder::header`、`HeaderPair::addHeaderPair`、`std::map/std::tree` 插入和分配在热点上。
- 当前 `B1/B14` benchmark 都是固定 `200 OK`、固定 body=`OK`、固定 keep-alive header。
- 这意味着 benchmark steady-state 每请求重复做“构造响应对象 -> 插 header -> 补 Content-Length -> 序列化”的工作，其中大部分在该场景下都可以提前完成。

**Design**

采用“两层最小化”方案：

1. 在 `HttpWriter` 增加“发送外部持有的连续字节视图”接口，避免为预构造响应再做一次内部复制。
2. 在 `B1-HttpServer` 和 `B14-HttpsServer` 中，把固定 `200 OK` 响应预构造成静态字节串，并在请求循环中直接复用。

这样做的原因是：

- 直接命中已知热点，去掉 builder/header/tree 插入和序列化的重复成本。
- 发送接口保持在 `HttpWriter` 内，避免 benchmark 直接绕过连接抽象。
- 改动只影响显式调用新接口的场景，不改变现有 `sendResponse()` 语义。

**Lifetime And Safety**

- 新接口只接受 `std::string_view`，并要求底层存储在异步发送完成前保持有效。
- 本轮 benchmark 使用静态响应字节串，生命周期覆盖整个进程，满足要求。
- 现有 `sendResponse()`、`sendRequest()`、`send(std::string&&)` 等拥有型发送路径保持不变。

**Verification**

先做定向验证：

1. 定向构建 `B1-HttpServer` / `B14-HttpsServer`
2. 运行相关 benchmark 或最小 smoke test
3. 若收益稳定，再补文档和提交

**Acceptance**

- `B1-HttpServer` 在固定响应场景下吞吐有稳定提升
- `B14-HttpsServer` 在同类场景下不退化，若有提升则一并记录
- 无新增功能性失败
