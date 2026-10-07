# API 参考

## 头文件和类型

主入口为 `<galay/cpp/galay-api/api_router.h>`；它包含类型化 builder 和绑定
模板。策略服务器使用 `api_server.h`，文档策略和独立文档安装使用
`docs.h`，独立 schema 使用 `schema.h`，OpenAPI 导出使用 `openapi.h`。
命名空间为 `galay::api`，协程返回
`galay::kernel::Task<ApiResult<T>>`。

| 类型或函数 | 用途 |
| --- | --- |
| `ApiResult<T>` | `std::expected<T, ApiError>` |
| `ApiError{code,message,status}` | 错误类别、具体原因、HTTP 状态 |
| `api_error_name(code)` | 所有公开错误码的稳定名称 |
| `ApiInfo{title,version,description}` | 文档基本信息 |
| `Operation` | operationId、说明、tags、成功状态及 `errors` |
| `InputBinding<I>` | `.path<&I::member>(name)`、`.query<&I::member>(name)` |
| `NoInput` / `NoContent` | 显式表示无输入 / 无 body 输出 |
| `ApiContext` | handler 完成前借用的只读 `HttpRequest&` |
| `PreparedApi` | 自持 router、`shared_ptr<const string>` document 和 endpoints |
| `DocsConfig` | spec/UI 路径，不包含资源目录 |
| `NoSwagger` | 默认空策略，不自动安装也不移除路由 |
| `HttpSwagger` | 按值持有 DocsConfig，默认安装内嵌官方 UI |
| `install_docs(api, config = {})` | 使用内嵌资源原子安装文档路由 |
| `install_docs_from_directory(api, config, directory)` | 显式加载完整文件资源，不使用 fallback |
| `ApiDocsPolicy` | 约束对象类型、可移动性和精确的 install 返回类型 |
| `ApiServer<Policy = NoSwagger>` | 启动前安装策略，随后持有 HTTP/1 server |

`ApiErrorCode` 枚举类型使用 `PascalCase`，枚举项使用 `k` 前缀加
`PascalCase`，例如 `ApiErrorCode::kBusinessError`、`ApiErrorCode::kBadRequest`；
函数仍使用 `snake_case`。`api_error_name(code)`
明确映射到稳定的 `business_error`、`bad_request` 等字符串，JSON 响应和
日志中的错误码名称不随 C++ 枚举项改名。

## Builder 和生命周期

```cpp
auto added = builder.add<galay::http::HttpMethod::GET, GetUserInput, UserDto>(
    "/users/:id", get_user,
    galay::api::Operation{.id = "getUser", .errors = {{404, "User not found"}}},
    galay::api::InputBinding<GetUserInput>{}
        .path<&GetUserInput::id>("id")
        .query<&GetUserInput::verbose>("verbose"));
if (!added) return std::unexpected(added.error());
return builder.build();
```

`add` 和 `build` 均检查结果。成功 build 后，再次 add/build 显式失败；
返回对象不借用 builder，builder 可以销毁。handler 对象、请求、context
和 input 在 `co_await` 完成前保持有效，但不能在任务完成后保留 context
中的引用。应用不要把 coroutine lambda 的临时闭包当作有效的长期协程
对象；优先使用独立协程函数，或生命周期覆盖完整任务的命名 callable。
输出优先使用自持 DTO。`string_view` 输出虽遵循 serde 的编码规则，但其
底层存储必须覆盖适配器编码完成；不能返回指向 handler 局部变量或其
按值 input 的 view，因为 handler 协程完成后这些存储可能已经销毁。
指向当前 `context.request` 的 view 仅在本次响应编码前有效，不能留给
后台任务或后续请求。可变 handler 捕获的跨请求线程安全由应用负责。

输入转换、handler 调度、业务结果和发送是不同的错误层。handler 返回
业务错误时，非 500 状态必须在 `Operation::errors` 登记，否则按 500 处理。
默认错误 JSON 为 `{"code":"business_error","message":"..."}`；描述文档
不会将未登记的业务状态谎报为稳定响应。

## Schema 和 OpenAPI

`schema_for<T>(SchemaUse::input/output)`、`schema_for_field` 和
`schema_json` 均返回 `ApiResult`。schema 是结构化模型，不是手工拼接的
JSON。数值保留 int64/uint64/double；optional 的可空性与字段存在性分开：
输入 optional 可缺失且可为 null，输出 optional 字段仍存在但允许 null。

DTO 使用具有成员指针的稳定 constexpr serde 描述符，枚举也要求稳定的
constexpr ADL descriptor。运行期的字段名、成员指针、约束及枚举编码和
合法集合必须与静态契约一致。`validate_contract(value)` 返回 `ApiResult<void>`，
检查当前对象和嵌套对象的描述符稳定性，不代替 serde 的值校验或编解码。
输入在绑定前后检查，输出在编码前检查；漂移返回明确的 500 错误，不会
输出与文档不同的 JSON。空容器及缺失 optional 仍检查其元素的类型契约；
可默认构造的 DTO 使用默认对象检查，没有建立新的反射或 schema 缓存。

`inspect_path` 检查 REST 路径并返回 OpenAPI 路径、同形键及参数名。
`validate_endpoint` 检查注册元数据和重复项；`render_openapi` 在启动前
生成确定性的 OpenAPI 3.1.0 JSON。首版 schema 全部内联，递归或不支持
类型显式失败，不通过 RTTI 推测公共组件名。

## 文档策略

`docs.h` 提供以下接口，均在 `galay::api` 中：

```cpp
struct NoSwagger {
    ApiResult<void> install(PreparedApi&) const noexcept;
};
class HttpSwagger {
public:
    explicit HttpSwagger(DocsConfig config = {});
    ApiResult<void> install(PreparedApi&) const;
};
template<class Policy>
concept ApiDocsPolicy = std::is_object_v<Policy> &&
    std::move_constructible<Policy> && requires(Policy& policy, PreparedApi& api) {
        { policy.install(api) } -> std::same_as<ApiResult<void>>;
    };
```

`NoSwagger` 直接返回成功，不加载资源、不新增路由，也不删除已通过
`install_docs` 手动安装的路由。它不关闭 builder 的 OpenAPI 生成。
`HttpSwagger{}` 使用默认文档路径和构建时内嵌的官方资源；可显式传入
`DocsConfig` 修改路径。唯一安装动作是调用 `install_docs(api, config)`；
不重新生成文档、不读文件，也不复制资源路由逻辑。

自定义策略只需满足 concept，无需继承或 virtual 方法。允许不可复制、
不可默认构造的策略，此时构造 server 必须显式移动传入该策略。
`install` 可为非 const，返回类型必须恰为 `ApiResult<void>`；没有
`prepare` 等第二个生命周期钩子，请求期间不调用策略。

## ApiServer

`api_server.h` 提供：

```cpp
template<ApiDocsPolicy Policy = NoSwagger>
class ApiServer {
public:
    explicit ApiServer(http::HttpServerConfig config = {}, Policy policy = Policy{});
    ApiResult<void> start(PreparedApi&& api);
    void stop();
    bool is_running() const noexcept;
    std::shared_ptr<const std::string> document() const noexcept;
};
```

使用 `http::HttpServerBuilder().build_config()` 传入配置，而非会构造底层
server 和 Runtime 的 `build()`。`ApiServer` 构造只保存配置和策略，不创建
Runtime；`start` 先验证 `PreparedApi` 并执行自持策略的 `install`，成功后
才创建 HTTP server 和 Runtime。server 不可复制或移动，也没有新增 virtual
方法。

- 未初始化阶段：`PreparedApi` 校验或策略安装失败时，原样返回错误，不创建
  Runtime、不监听，也不把 router 交给底层 HTTP server。`ApiServer` 仍可
  再次调用 `start`。`HttpSwagger` 的失败是原子的，不留下部分文档路由；
  修正路径等预检失败原因后，可将同一个未消费的 `PreparedApi` 再次用
  `std::move` 传入重试。显式文件策略可在修复资源文件后重试。
  自定义策略的失败副作用无法被通用 server 回滚，实现者必须自行保证失败
  不留下半注册状态；是否可重试取决于该策略自己的保证。
- 单次初始化阶段：策略安装成功后，实例创建底层 server 并调用底层启动；
  从此实例只能使用一次，包括监听失败、运行中或 `stop` 之后。即使监听
  失败，后续 `start` 也返回 `ApiErrorCode::kServerError`；`stop` 不支持 restart。
- 底层失败：首次底层启动失败返回 `ApiErrorCode::kTransportError`，此时 router 已消费，
  不能重用原 `PreparedApi`。重启必须重新 build API 并创建新 server。
- `document()`：构造后或预检/安装失败时为空，策略安装成功后、底层启动前
  就保存同一份不可变共享文档；底层监听失败或 `stop()` 后仍可读取。调用方
  应检查指针后再解引用。

策略对象按值自持且地址稳定，底层 HTTP server 在策略销毁前停止并销毁。
自定义 handler 可借用 server 自持策略，但不能借用调用方临时策略、
builder 或 `install` 参数的 `PreparedApi&`。文档和资源应捕获不可变共享
对象，不能依赖局部变量或已被移动的 prepared 对象。

文档只包含同一个 `ApiBuilder` 登记的全部 typed endpoint；不自动反射
裸 `HttpRouter::add_handler`，也不把文档策略新建的非 typed 路由虚构成
DTO 操作。`ApiServer` 是 `galay-api` 的上层组合，不更改底层 `HttpServer`
模板或为 `galay-http` 增加 serde / Swagger 依赖。

## 独立文档安装

```cpp
struct DocsConfig {
    std::string spec_path = "/openapi.json";
    std::string ui_path = "/docs";
};
ApiResult<void> install_docs(PreparedApi& api, const DocsConfig& config = {});
ApiResult<void> install_docs_from_directory(PreparedApi& api, const DocsConfig& config,
                                          const std::string& directory);
```

应用直接安装内嵌文档的用法：

```cpp
galay::api::DocsConfig config{
    .spec_path = "/openapi.json",
    .ui_path = "/docs"};
auto result = galay::api::install_docs(prepared, config);
if (!result) return std::unexpected(result.error());
```

`install_docs` 仍可独立与 `PreparedApi` / 原有 `HttpServer` 组合使用，
不是兼容层。使用 `ApiServer<HttpSwagger>` 时不要另行预安装，否则策略会
返回重复安装错误。`DocsConfig` 的默认值是 `spec_path="/openapi.json"`、
`ui_path="/docs"`；它只描述路由，不负责选择资源来源。

`install_docs` 使用构建时已校验并内嵌的 `swagger-ui-dist@5.17.14`；
缺失或损坏的固定上游资源在构建期报错，不在运行期搜索目录。
`install_docs_from_directory` 只使用显式目录：自定义策略需提供下列九个
非空普通文件，单个文件不超过 16 MiB：

```text
swagger-ui.css
swagger-ui-bundle.js
swagger-ui-standalone-preset.js
favicon-16x16.png
favicon-32x32.png
LICENSE
NOTICE
README.md
SHA256SUMS
```

其中 `README.md` 保留版本和来源，`SHA256SUMS` 记录上游文件的校验信息；
文件安装器不替自定义资源执行构建期的官方哈希校验。缺文件、空文件、
非普通文件、超限或读取/关闭失败返回 `ApiErrorCode::kResourceError`，保留具体原因。
任何缺项都不从内嵌资源或 CDN 补齐；完整自定义策略示例见使用指南。

两种安装器均在启动服务前调用一次。全部必要资产和初始化配置先准备 / 序列化，
随后完整预检文档、UI、初始化脚本和资产路径的自冲突及 endpoint/router 冲突，
成功后才注册。非法路径、冲突或重复安装均显式失败；失败不留下部分文档
路由。UI 路径下的九个资源（包括许可和来源文件）也参与冲突预检；默认
路径下可访问 `/docs/LICENSE`、`/docs/NOTICE`、`/docs/README.md` 和
`/docs/SHA256SUMS`。文件策略在修复资源后可重试失败的安装。

成功后 `document` 和资产均以不可变共享状态持有。请求热路径不读文件、
不生成 schema、不修改注册表，也不使用阻塞锁。普通 lambda 调用独立
协程函数并按值传递共享资源，复制 route handler 或移动 router 不会使
资产失效。发送结果和异常连接清理结果均检查；正常连接关闭仍由
HTTP server 管理。
