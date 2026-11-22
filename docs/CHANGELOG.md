# 更新日志

# 0.1.0 — 2026-11-22

### 新增
- 契约组件装配：`Component` / `components` / `dependencies`、编译期闭包、
  组件 `hooks`、`app_beans_t` / `app_hooks_t`。
- 定时任务拆为契约组件 `cboot.schedule`，支持固定延迟和 5/6 段 cron 表达式。
- 编译期 DI：`[[=Bean{}]]`、构造器注入、拓扑排序、生命周期与编译期诊断。
- `[[=Named{"name"}]]` 同基类多实例；`[[=Value{"key"}]]` 标量配置注入。
- `[[=Profile{"name"}]]` 条件装配：`cboot.profiles.active` 控制 bean 是否构造/挂载。
- JSON：反射绑定/流式序列化；`std::map` / `std::unordered_map<std::string,V>`
  ↔ object；`json::value` 直接读写；`json::codec<T>` 自定义类型编解码；
  解析错误带行/列，嵌套深度默认 128（`parse_options` 可调）。
- JSON 配置、REST 路由、编译期 AOP、协程 handler。
- `[[=EventListener{}]]` + `ctx.publish` / 注入 `event_publisher`：同步事件派发、
  顺序稳定、异常隔离、可接切面。
- 内建 `/health`（200/503 + JSON）与 `/beans`（仅 dev）；`health_registry` 可标记 DOWN。
- threads / IOCP / epoll 三后端：keep-alive、pipelining、背压、超时、优雅停机。
- `application<App>` 自动发现、`find_package(cboot)`。

### 重构
- 目录平铺：`modules/` + `src/` → `packages/<pkg>/`；每个包自带
  `CMakeLists.txt`、模块接口单元与 `src/`。
- 新增 `cboot_package()` / `cboot_component()` 与包配置模板；
  基础包统一导出 `CbootTargets`，契约组件可独立 `find_package(cboot.<name>)`。
- 公共 target 统一为 `cboot::<name>`；移除旧 `cboot::cboot_*` 兼容名。
- 模块安装到 `share/cboot/modules/<pkg>/`；模块 provider 必须直接链接。

### 性能
- 流水线 1.25M plaintext / 0.94M JSON（1 worker），2.11M / 2.07M（8 worker）。
- 小 JSON 响应零堆分配；同级 Java 对照每核 +13% / +17%。

### 修复
- IOCP 失败完成包状态校验与 accept 链自愈。
- 压测客户端 RST 关闭，避免 TIME_WAIT 耗尽临时端口。

### 测试
- 24 项 ctest，Debug/Release 全绿；14 个负面用例锁定诊断。
