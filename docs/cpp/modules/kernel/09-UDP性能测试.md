# 09-UDP性能测试

本页现在只保留 UDP 数据面的补充定位；当前 fresh 性能事实请优先回到主干页。

## 本页回答什么

- 仓库里有哪些 UDP 相关 benchmark / test / example
- 哪些结果是当前 fresh evidence，哪些只是入口保留
- 先去哪里看 `UdpSocket` 的真实用法

## 当前稳定事实

- `UdpSocket` 是公开 API，接口与边界以 `docs/02-API参考.md` 为准
- 2026-03-15 已重新执行本地 kqueue fresh UDP 验证
- `T5-udp_socket`、`T6-udp_server`、`T7-udp_client` 已纳入全量 `test matrix` 并通过
- `E5-UdpEcho` 已 fresh 运行通过
- `B4/B5-Udp` 已恢复有效收发；当前本地 fresh 结果为 `100000 sent / 99505 received`，loss `0.495%`
- `B5-UdpClient` 仍只作为 smoke / stability 检查，最终 UDP 性能签收以 `B6-Udp` 为准
- `B6-Udp` 当前本地 fresh 结果为 `200000/200000`、loss `0.00%`、recv throughput `8.85691 MB/s`

## 先看主干页

- API 与边界：`docs/02-API参考.md`
- UDP 工作流：`docs/03-使用指南.md`
- 当前性能事实：`docs/05-性能测试.md`
- 平台与 IO 背景：`docs/06-高级主题.md`

## 源码 / 验证锚点

- 源码：`galay-kernel/async/udp_socket.h`、`galay-kernel/async/udp_socket.cc`
- 测试：`test/t5_udp.cc`、`test/t6_udp.cc`、`test/t7_udp.cc`
- 示例：`examples/include/e5_udp.cc`
- benchmark：`benchmark/b4_udp.cc`、`benchmark/b5_udp.cc`、`benchmark/b6_udp.cc`

## RAG 关键词

- `UdpSocket`
- `UDP echo`
- `B4-UdpServer`
- `B5-UdpClient`
- `B6-Udp`
- `T5-udp_socket`
- `E5-UdpEcho`
