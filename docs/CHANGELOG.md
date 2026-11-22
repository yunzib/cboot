# 更新日志

# 0.1.0 — 2026-11-22

### 新增
- 编译期 DI：`[[=Bean{}]]`、构造器注入、拓扑排序、生命周期与编译期诊断。
- `[[=Named{"name"}]]` 同基类多实例；`[[=Value{"key"}]]` 标量配置注入。
- `[[=Profile{"name"}]]` 条件装配：`cboot.profiles.active` 控制 bean 是否构造/挂载。
- JSON 配置、反射 JSON、REST 路由、编译期 AOP、协程 handler。
- threads / IOCP / epoll 三后端：keep-alive、pipelining、背压、超时、优雅停机。
- `application<App>` 自动发现、`find_package(cboot)`。

### 性能
- 流水线 1.25M plaintext / 0.94M JSON（1 worker），2.11M / 2.07M（8 worker）。
- 小 JSON 响应零堆分配；同级 Java 对照每核 +13% / +17%。

### 修复
- IOCP 失败完成包状态校验与 accept 链自愈。
- 压测客户端 RST 关闭，避免 TIME_WAIT 耗尽临时端口。

### 测试
- 19 项 ctest，Debug/Release 全绿；7 个负面用例锁定诊断。
