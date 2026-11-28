# 更新日志

# 0.2.0 — 2026-11-28

### 新增
- 请求关联层 `cboot.web.access`：内建 `AccessLog` 全局中间件
  （顺序 -950，ErrorHandler 外层）——request_id 解析/生成（复用合法
  `X-Request-Id`，否则 32 位十六进制随机值）、响应头回写、
  `request_context` 属性与日志 MDC 注入、单行 JSON 访问日志
  （method/path/status/duration_ms/request_id/route，404/405 记录
  最终状态码）；`application` 启动时自动安装；关闭走
  `log.logger_levels` 的 `cboot.web.access` 前缀覆盖。
- 指标注册表 `cboot.core.metrics`：`counter` / `histogram`（固定桶、
  累积口径、原子无锁观测）/ `gauge`（原子 double），进程级单例
  `metrics::global()`、Prometheus 文本格式渲染（名称排序、系列终身有效）。
- schema 迁移 `cboot.orm.migration`：`MigrationRunner`（Flyway 前进式子集）
  ——版本表 `cboot_schema_version`（version UNIQUE/description/applied_ms）、
  按版本升序执行未应用脚本、脚本与版本行同事务、失败回滚并抛带版本号的
  `orm::error`、重复启动幂等、多实例并发靠版本行唯一约束防重复执行；
  `orm_component` 贡献 `MigrationRunner` bean。
- k8s 三探针端点：`GET /livez`（恒 200，不看依赖）、
  `GET /readyz`（依赖健康 200 READY / 503 NOT_READY + reason）、
  `GET /startupz`（恒 200，容器构造完成），随 `/health` 自动挂载。
- POSIX 优雅停机：`SIGTERM` / `SIGINT` → 自管道 → 看门线程调用
  `server::stop()`（信号处理器仅 `write`，async-signal-safe）；
  Windows 控制台事件路径不变。
- 请求限流 `cboot.web.ratelimit`：`RateLimiter` 固定窗口中间件
  （route/client_ip/route_ip/global 维度、`remote_addr` 属性或
  X-Forwarded-For/X-Real-IP 推断客户端、429 + Retry-After、
  `web_rate_limited_total` 指标、可注入时钟）。
- 双 token 认证：`auth::token_pair`、`session.kind`（access/refresh）、
  `AuthService::login_pair/refresh/logout_pair`、`refresh_timeout_s` /
  `rotate_refresh` 配置；refresh 轮换、种类隔离（refresh 不能当请求
  凭证）；`PgTokenStore` 增补 `kind` 列并自动给旧表 ALTER。
- ORM 驱动连接池借出前活性校验：空闲超过 `idle_probe_s`（新配置，
  默认 30，0 关闭）时一次真实探测（PG 空查询往返 / MySQL
  `mysql_ping`）+ 本地状态检查；死连接销毁重建（池容量不变）。
- ORM 观测：慢 SQL 日志（`slow_query_ms` 新配置，默认 200，
  0 关闭；`cboot.orm.pg` / `cboot.orm.mysql` logger，SQL 截 256 字符）
  与指标系列 `orm_pool_connections`{dialect,state} /
  `orm_pool_reconnects_total` / `orm_queries_total` /
  `orm_query_duration_ms`{dialect}（`cboot.core.metrics` 注册表）。
- 内建指标端点 `GET /metrics`：请求系列 `http_requests_total`
  {method,route,status 大类} 与 `http_request_duration_ms`
  {method,route}（404 记 route="-" 防标签爆炸，系列指针按方法+路由
  缓存）+ 应用 gauge（cboot_beans/startup_ms/uptime_ms）。
- 生命周期切面：`join_point` / `call_info.point`、
  `context(with_lifecycle_aspects, cfg, 切面...)` 把每个 bean 的
  `PostConstruct` / `PreDestroy` 包进 `aspect_chain`；
  `App::lifecycle_aspects` / `app_lifecycle_t` 接入应用引导。
- 方法级切面选择：切面 Bean `[[=web::Aspect{"name"}]]`、路由/控制器级
  `[[=web::UseAspect{"name"}]]`、注册表协议 `web::AspectRegistry`、
  `mount_routes()`；类级对全部路由生效、方法级追加、重名去重，
  层次为全局 → 类级 → 方法级；未注册名 / 无注册表 mount 均编译错。

### 变更
- `PreDestroy` 抛异常改为隔离并记录（原先析构路径抛出即 terminate）。
- `cboot.core` 依赖 `cboot.aop`（`packages/CMakeLists.txt` 中 aop 先于 core）。

### 修复
- 空 bean 清单 `context<>` 无法编译（零长度数组取下标）。
- `aspect_chain` 持引用元素时构造失败（`std::move` → `std::forward`）。

### 测试
- 40 项 ctest，Debug/Release 全绿；新增 `lifecycle_aspect_smoke` /
  `method_aspect_smoke`，负面用例增至 16 个。

# 0.1.0 — 2026-11-23

### 新增
- 通用 ORM 方言抽象 `dialect` 与 MySQL 驱动 `cboot.orm.mysql`
  （`MysqlProps` / `MysqlDataSource` / `mysql_component`）。
- DTO 校验基础包 `cboot.validation`：`NotBlank` / `Range` 注解、
  `validate` / `validate_or_throw`；HTTP body 绑定失败返回 400 +
  字段错误列表。
- 请求中间件：`web::Middleware` Bean 角色、全局/路由级链、
  `Request` headers、`request_context`、`OPTIONS`。
- 认证组件：`cboot.auth` 登录态（opaque token、AuthService、
  Memory/PostgreSQL TokenStore、路由策略、认证事件）；`cboot.jwt`
  独立提供 HS256/RS256 校验。
- ORM 事务 `EntityManager::transaction` + 固定连接 `session`；
  原生 SQL `query<T>` 按结果列名映射，驱动 statement 暴露列元数据。
- 认证 RBAC：`RequireRole` / `RequireScope` 方法级/控制器级注解、
  `auth_policy_hook` 策略合并、403 与 `authorization_failed` 事件。
- 认证传输与续期：`token_cookie` Cookie 回退、`attach_cookie` /
  `clear_cookie`；`active_timeout_s` 滑动续期与 `TokenStore::touch`。
- CORS：可选 `cors_component`（来源白名单/通配、预检 204、方法/
  请求头校验、凭证与暴露头）。
- 全局错误层 `cboot.web.error`：`error_info` / `ErrorHandler` /
  `ErrorMapper`，统一 404/405、校验 400、认证 401/403、handler 500；
  `request_context::get<T>()`；AuthGuard 改为抛结构化 `web::error`。
- 日志 `cboot.log`：`logging::logger` / sink / text/JSON / MDC；
  可选 `log_component` 支持 console/file/rolling、级别覆盖；
  app 启动日志与 ErrorHandler 5xx 日志接入。
- 配置源：CLI `--key=value` / `--config`、`CB_` 环境变量，优先级
  CLI > env > JSON；配置绑定错误聚合全部非法键。
- `cboot.auth.postgres`：`PgTokenStore`（表 `cboot_auth_sessions`）；
  auth 拆分核心 / memory / postgres TokenStore 组件。

### 重构
- ORM 拆分为通用层 `cboot.orm` 与驱动 `cboot.orm.postgres` /
  `cboot.orm.mysql`，通用层不再链接数据库客户端。
- 组件构建支持 `PACKAGE_NAME`，驱动 target 与包名解耦。
- 基础包 `cboot_package` 支持 `VERSION`（默认 `CBOOT_VERSION`），
  安装配置显式导出 `cboot_VERSION` / `CBOOT_VERSION`。

# 0.1.0 — 2026-11-22

### 新增
- 契约组件装配：`Component` / `components` / `dependencies`、编译期闭包、
  组件 `hooks`、`app_beans_t` / `app_hooks_t`。
- 定时任务拆为契约组件 `cboot.schedule`，支持固定延迟和 5/6 段 cron 表达式。
- 契约组件 `cboot.orm`（PostgreSQL/libpq）：`Table` / `Column` / `Id` 实体元数据、
  CRUD、类型安全 select/update/delete DSL（含 IN/LIKE/ILIKE）、连接池与 health；
  `db` 配置的 password 支持留空 / `${PGPASSWORD}` / 环境变量。
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
- 25 项 ctest，Debug/Release 全绿；14 个负面用例锁定诊断。
