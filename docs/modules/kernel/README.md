# 文档导航

## 当前基线

- 真相优先级：公开头文件 > 实现 > `examples/` > `test/` > `benchmark/` > Markdown
- 当前已验证环境：macOS / AppleClang 17 / 默认 Release / kqueue
- 当前未验证但保留的资产：C++23 import 示例；它们是真实源码文件，但本轮环境未生成对应 target
- 主检索面固定为 `docs/00-快速开始.md` 到 `docs/07-常见问题.md`
- `docs/08-计算调度器.md` 到 `docs/20-异步同步原语.md` 现在只作为补充专题落地页保留，负责摘要、关键词、源码锚点和验证入口，不再承担完整主体叙述

## 两层规则

- 先回答：优先在 `docs/00-快速开始.md` 到 `docs/07-常见问题.md` 内给出结论、API、边界与用法
- 再定位：只有当问题已经落到某个子系统、平台后端或交付环节时，再进入 `docs/08-计算调度器.md` 到 `docs/20-异步同步原语.md`
- 先主干、后补充的默认顺序对人工阅读与 RAG 检索都适用

## 建议阅读顺序

1. `docs/00-快速开始.md`
2. `docs/01-架构设计.md`
3. `docs/02-API参考.md`
4. `docs/03-使用指南.md`
5. `docs/04-示例代码.md`
6. `docs/05-性能测试.md`
7. `docs/06-高级主题.md`
8. `docs/07-常见问题.md`

## 补充专题页

仅当查询已经明确落到某个运行时子系统、平台后端或交付场景时，再进入以下页面：

- `docs/08-计算调度器.md`
- `docs/09-UDP性能测试.md`
- `docs/10-调度器.md`
- `docs/11-协程.md`
- `docs/12-网络IO.md`
- `docs/13-文件IO.md`
- `docs/14-并发.md`
- `docs/15-定时器调度器.md`
- `docs/16-环形缓冲区.md`
- `docs/17-零拷贝发送文件.md`
- `docs/18-运行时Runtime.md`
- `docs/19-文件监控.md`
- `docs/20-异步同步原语.md`

## 按任务进入

- 想先构建并跑通：`docs/00-快速开始.md`
- 想确认公开 API：`docs/02-API参考.md`
- 想问模块门面、`import galay.kernel`、后端裁剪边界：`docs/02-API参考.md`
- 想查 `Bytes` / `Buffer` / `RingBuffer` / `Host` / `IOError` 细节：`docs/02-API参考.md`
- 想照着真实源码上手：`docs/03-使用指南.md`、`docs/04-示例代码.md`
- 想看 benchmark 与当前结果：`docs/05-性能测试.md`
- 想看模块、后端、绑核等高级话题：`docs/06-高级主题.md`
- 想看安装、`find_package` 与 CI 验证：`docs/00-快速开始.md`、`docs/02-API参考.md`、`docs/07-常见问题.md`

## 主题回流矩阵

- 模块门面 / Runtime / Task / 定时器：主干看 `docs/01-架构设计.md`、`docs/02-API参考.md`、`docs/03-使用指南.md`；补充页只负责定位 `docs/08-计算调度器.md`、`docs/10-调度器.md`、`docs/11-协程.md`、`docs/15-定时器调度器.md`、`docs/18-运行时Runtime.md`
- 地址 / 错误 / 缓冲区：主干看 `docs/02-API参考.md`、`docs/03-使用指南.md`、`docs/07-常见问题.md`；补充页只负责定位 `docs/16-环形缓冲区.md`
- 网络 / 文件 / 零拷贝 / 文件监控：主干看 `docs/02-API参考.md`、`docs/03-使用指南.md`、`docs/05-性能测试.md`、`docs/06-高级主题.md`；补充页只负责定位 `docs/09-UDP性能测试.md`、`docs/12-网络IO.md`、`docs/13-文件IO.md`、`docs/17-零拷贝发送文件.md`、`docs/19-文件监控.md`
- 并发 / 同步原语：主干看 `docs/01-架构设计.md`、`docs/02-API参考.md`、`docs/03-使用指南.md`、`docs/07-常见问题.md`；补充页只负责定位 `docs/14-并发.md`、`docs/20-异步同步原语.md`
- 企业接入 / 安装消费：主干看 `docs/00-快速开始.md`、`docs/02-API参考.md`、`docs/07-常见问题.md`

## 按关键词进入

- `galay.kernel` / `module_prelude.hpp`：`docs/02-API参考.md`
- `Runtime` / `RuntimeBuilder`：`docs/02-API参考.md`、`docs/18-运行时Runtime.md`
- `ComputeScheduler` / `EpollScheduler` / `KqueueScheduler` / `IOUringScheduler`：`docs/02-API参考.md`、`docs/08-计算调度器.md`
- `Task` / `TaskRef` / `then` / `co_await` / `sleep`：`docs/02-API参考.md`、`docs/11-协程.md`
- `Bytes` / `ByteMetaData` / `Buffer` / `RingBuffer`：`docs/02-API参考.md`、`docs/16-环形缓冲区.md`
- `Host` / `IPType` / `IOError`：`docs/02-API参考.md`
- `TcpSocket` / `UdpSocket`：`docs/02-API参考.md`、`docs/12-网络IO.md`
- `AsyncFile` / `AioFile` / `sendfile`：`docs/02-API参考.md`、`docs/13-文件IO.md`、`docs/17-零拷贝发送文件.md`
- `FileWatcher`：`docs/02-API参考.md`、`docs/19-文件监控.md`
- `AsyncMutex` / `MpscChannel` / `UnsafeChannel` / `UnsafeChannelWakeMode` / `AsyncWaiter`：`docs/02-API参考.md`、`docs/14-并发.md`、`docs/20-异步同步原语.md`
- `TimerScheduler` / `CBTimer`：`docs/02-API参考.md`、`docs/15-定时器调度器.md`
