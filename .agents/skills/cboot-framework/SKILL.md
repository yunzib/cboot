---
name: cboot-framework
description: 在 cboot-framework 里开发或修改基于 C++26 静态反射的应用框架代码。当用户要新增 Bean、控制器、REST 路由、配置绑定、切面、协程 handler，排查编译期依赖注入/注解/模块错误，或改动 packages/、cmake/、examples/ 时使用；包含最小应用、注解与 DI 规则、路由参数协议、配置键、包/CMake 约定、构建测试命令，以及 GCC 16 反射/模块的已知约束。
---

# cboot-framework 框架开发

cboot-framework 把 Spring 的运行时装配搬到编译期：注解在编译期被反射读取，依赖图、路由表、
配置绑定在启动前确定；运行时只做 I/O、解析和序列化。改动时优先保持这个边界，
不要把注册/扫描/装配搬到运行期。

## 仓库与工具链

- 主工具链只有 GCC 16.2（niXman MinGW），C++26 + `-freflection` + `import std`；
  Clang/MSVC 没有 P2996 实现，不要为它们加兼容分支。
- 平铺包结构：`packages/<pkg>/`；接口单元 `packages/<pkg>/*.cppm`
  （`export module`），实现单元 `packages/<pkg>/src/*.cpp`。
  每个包有自己的 `CMakeLists.txt`；`packages/CMakeLists.txt` 只做同级
  `add_subdirectory` 注册，不写源文件/安装/测试细节。
- 两类包：
  - **基础包**：`meta/json/config/core/log/aop/async/validation/web/app`，随 `cboot`
    一起安装导出，不参与应用 `components` 选择；
  - **契约组件**：`schedule`、`orm`、`orm.postgres`、`orm.mysql`、
    `jwt`、`auth`、`auth.postgres` 等，独立包 `cboot.<name>`；驱动组件可用 `PACKAGE_NAME`
    解耦 target 名与包名（`orm_postgres` / `cboot.orm.postgres`）。
    `web` 包内另有可选 `cors_component`（`import cboot.web.cors`）。
    `log` 包内另有可选 `log_component`（`import cboot.log.component`）。
    `auth` 与 `jwt` 相互独立。
    描述符声明 `beans` / `dependencies` / `hooks`，由应用 `components` 选择。
    首个真实组件是 `cboot.schedule`（`Scheduled` 注解 + 调度 hook）。
- `core` 是基础层，不是其他包的容器；依赖由 `target_link_libraries` 表达。
- 模块分层：`app → web.server → routing/http → core/json/aop → meta`。
  契约组件依赖 `core`/`config`（需要路由时依赖 `web`），**不要依赖 `app`**；
  新增依赖时沿分层方向，不要反向 import。
- 应用用 `using components = type_list<...>;` 选择根组件；框架在编译期算
  依赖闭包，`app_beans_t` 合并扫描 bean + 组件 bean，`app_hooks_t` 收集 hooks。
- 公共 target 名统一 `cboot::<name>`（如 `cboot::app`）；构建 target 名是
  `cboot_<name>`。旧 `cboot::cboot_app` 等兼容名已删除，不要恢复。
- 单入口：`import cboot.app;` 已 re-export 全部框架词汇（Bean/Inject/路由/
  Request/Response/json_response/application/...）。示例里也可以按需 import 子模块。
  契约组件模块按需 import `cboot.core.*` / `cboot.meta.reflect` / `cboot.config`，
  不要 import `cboot.app`（app 是装配器/组合根）。

构建与测试：

```bash
cmake --preset gcc && cmake --build --preset gcc && ctest --preset gcc
cmake --preset gcc-release && cmake --build --preset gcc-release
ctest --test-dir build-release --output-on-failure
```

改动框架后 Debug/Release 都要过 32 项 ctest（MySQL + OpenSSL + PostgreSQL 启用时；
裁剪时相应测试会跳过）；
负面用例（`negative_*.cpp`）是预期编译失败，不能“修好”。

外部消费口径：

```cmake
find_package(cboot REQUIRED)
target_link_libraries(app PRIVATE cboot::app)   # 模块 provider 必须直接链接
```

`examples/standalone` 是独立消费工程，默认 `CBOOT_SDK_DIR` 指向
`<repo>/build-release/install`；根构建不再编译 examples。

## 包与 CMake 约定

- 新增基础包：

```cmake
# packages/<pkg>/CMakeLists.txt
cboot_package(<pkg>
    VERSION ${CBOOT_VERSION}   # 基础包共享 SDK 版本，可省略
    TIER foundation
    MODULES <pkg>.cppm
    SOURCES src/<pkg>.cpp
    DEPENDS meta json config)
```

  然后在 `packages/CMakeLists.txt` 按依赖顺序 `add_subdirectory(<pkg>)`；
  不要把所有包塞进一个 CMakeLists。

- `cboot_package()` 建 `cboot_<pkg>` target、别名 `cboot::<pkg>`、
  `EXPORT_NAME <pkg>`；模块文件集 `cxx_modules`，实现源走 PRIVATE，
  依赖只写包名并由 helper 解析成 `cboot::<dep>`。
- 基础包统一进共享 `CbootTargets`；根 CMake 在所有基础包定义完后
  只 `install(EXPORT CbootTargets)` 一次。
- 契约组件用 `cboot_component(...)`：独立导出 `Cboot<Name>Targets`、
  独立包 `cboot.<name>`，包配置先 `find_dependency(cboot)`。
  组件可贡献 `beans`，也可贡献 `hooks`；hook 类型提供嵌套
  `template<class Ctx> struct instance`，`boot_runner` 在 context 构造完后
  安装、析构前停止。直接 `context` 测试时用
  `hook_scope<decltype(ctx), Hook> hooks(ctx);`，按 `Hooks...` 顺序构造。
- 模块安装到 `share/cboot/modules/<pkg>/`；包配置在
  `share/cmake/cboot`（基础包）与 `share/cmake/cboot.<name>`（组件）。
- 模块 provider 必须被消费方**直接链接**；interface 别名不能作为模块 provider。
- 跨包回归测试在根 `test/`；包内测试放 `packages/<pkg>/test/`，
  通过 `TEST_SOURCES` 注册。

## 最小应用

```cpp
import cboot.app;
import std;

struct UserDto { std::string name; int age{}; };

struct [[=Bean{"repo"}]] UserRepo {
    UserDto find(int id) {
        return {.name = "user" + std::to_string(id), .age = 20 + id};
    }
};

struct [[=Bean{}]] UserApi {
    UserApi(UserRepo& repo) : repo_(repo) {}       // 构造器注入

    [[=Get{"/users/{id}"}]]                         // 路径参数 → int
    Response get(int id) { return json_response(repo_.find(id)); }

    [[=Post{"/users"}]]                             // body → DTO
    Response create(const UserDto& in) { return json_response(in); }

    UserRepo& repo_;
};

struct App : application<App> {
    static constexpr std::string_view config_path = "app.json";  // 可选
    static constexpr unsigned short default_port = 8080;         // 可选
};

int main() { return App{}.run(); }
```

`App` 类型本身就是自动发现锚点：全局命名空间中当前 TU 可见、标注了
`[[=Bean{}]]` / `[[=Config{...}]]` 的完整类型会被自动纳入，无需清单。
可见性受模块导出影响：跨模块的 bean 必须由模块 `export`。
扫描容量上限 128；超过时拆应用或改用显式清单。

## 注解规则（GCC 16 / P3394 实测约束）

| 注解 | 位置 | 语义 |
|---|---|---|
| `[[=Bean{}]]` / `[[=Bean{"name"}]]` | 类型 | 注册为 bean；可选名字用于诊断 |
| `[[=Config{"prefix"}]]` | 类型 | 配置 bean，由 `config::bind<T>` 构造 |
| `[[=Inject{}]]` | 构造器 | 多构造器时指定注入用者 |
| `[[=Named{"name"}]]` | 构造器参数 | 多个同基类实例中按 bean 名选择 |
| `[[=Value{"key"}]]` | 构造器参数 | 直接注入标量配置（bool/数值/string/枚举） |
| `[[=Profile{"name"}]]` | 类型 | 仅当 profile 激活时装配 |
| `[[=EventListener{}]]` | 方法 | 订阅事件；参数类型即事件类型 |
| `[[=Scheduled{ms}]]` / `[[=Scheduled{.expression="..."}]]` | 方法 | 定时任务（契约组件 `cboot.schedule`；public/非静态/无参数） |
| `[[=PostConstruct{}]]` | 成员函数 | 构造后、依赖者构造前调用 |
| `[[=PreDestroy{}]]` | 成员函数 | 逆拓扑序销毁 |
| `[[=Get{"/p"}]]` 等 | 成员函数 | 路由：Get/Post/Put/Delete/Patch |

硬规则：

- 注解值必须是**值表达式**：`[[=Bean{}]]`、`[[=Get{"/x"}]]`。裸 `[[=Bean]]`
  会被静默忽略，不要省略花括号。
- 位置在 class-key 之后：`struct [[=Bean{}]] X`、`class [[=Bean{}]] X`。
- 注解值类型必须 structural（全公开、无用户构造器）。字符串用 `bean_name`
  （`char[64]`）或 `path_name`（`char[128]`），不要用 `std::string` /
  `std::string_view` 作注解参数；超长字面量编译错。
- 控制器没有独立注解：控制器也是 bean，必须显式 `[[=Bean{}]]`。
  有路由方法却漏标 → 编译错并指出类型名（不要加回隐式规则）。
- 不设 Service/Component/Controller 角色注解。分层写在文档里，不写成注解；
  需要新语义时优先给 `Bean` 加字段，而不是新增角色类型。
- `struct` 和 `class` 都可用；`class` 的构造器、handler、生命周期函数必须 public。

## 依赖注入

构造器选择顺序：

1. 恰好一个 `[[=Inject{}]]` 构造器 → 用它；
2. 否则只有一个构造器 → 用它；
3. 否则有默认构造器 → 用默认构造；
4. 多个候选且无标注 → 编译错，提示加 `[[=Inject{}]]`。

- 依赖按类型注入，参数写成 `T&`；类型可以是接口基类，候选包含同类型与派生类型。
- 同基类多实例用 `[[=Named{"name"}]]` 选择；未限定参数取唯一候选，或唯一默认
  实例（`[[=Bean{}]]` 无名），否则编译错并列出候选：

```cpp
struct [[=Bean{"primary"}]] Primary : DataSource {};
struct [[=Bean{"replica"}]] Replica : DataSource {};
struct [[=Bean{}]] DefaultSource : DataSource {};

struct [[=Bean{}]] Svc {
    Svc([[=Named{"primary"}]] DataSource& p,
        [[=Named{"replica"}]] DataSource& r);
};
```

- 运行时取用：`ctx.get<DataSource>("primary")`、`ctx.get<DataSource>()`（默认）、
  `ctx.get_at<I>()`（按清单下标）。
- 多构造器时显式标注注入用者：

```cpp
struct [[=Bean{}]] Svc {
    [[=Inject{}]] Svc(Repo& repo, DbProps& cfg);
    Svc();                       // 多个候选，必须用 Inject 指定
};
```

- 配置 bean 不经过构造器：`[[=Config{"db"}]]` 的类型由
  `config::bind<T>(props, "db")` 生成，其他 bean 直接注入 `T&`。

```cpp
struct [[=Config{"db"}]] DbProps {
    std::string host{"localhost"};
    int port{5432};
};
```
- 需要原始配置时注入 `const config::properties&`。
- 标量参数用 `[[=Value{"db.pool_size"}]] int pool` 直接注入；支持
  bool/整数/浮点/string/枚举，参数按值或 const&。缺配置/缺键/值非法 →
  启动期报错；非标量、空 key、与 `Named` 同用 → 编译错。Value 参数不参与依赖图。
- `[[=Profile{"dev"}]]` 按 `cboot.profiles.active`（逗号分隔）条件装配：
  未激活 bean 不构造、不挂载、不可注入；激活 bean 依赖未激活 bean → 启动期报错。
- 生命周期：先按拓扑序构造全部 bean，再按同序调用 `PostConstruct`；
  停机逆序 `PreDestroy`。依赖者的 `PostConstruct` 一定在被依赖者之后。
- 编译期诊断：缺失依赖给出 `'A' requires 'B'`；循环给出
  `circular dependency: A -> B -> A`；私有构造器/多 `Inject` 都编译错。
- 测试或需要精确控制 bean 集合时，用逃生口：
  `run<Beans...>(port)` 或 `context<Beans...>`，不要为了测试改自动扫描。

## 路由

- 路径模板 `/users/{id}/{field}`，`{...}` 段按出现顺序消费 handler 参数。
- handler 参数按声明序解释：
  - `Request` → 请求本体（method/path/query/body 都是视图，只在本次调用内有效）；
  - 聚合体（struct）→ JSON body 反序列化；
  - 数值/字符串 → 路径参数（数值走 `from_chars`）。
- 返回 `Response`；也可以返回 `task<Response>`（dispatch 内 `sync_wait`）。
  协程 handler 只能做“会自行完成”的链式异步；不要 co_await 依赖服务器循环
  喂事件的对象，否则 `sync_wait` 会明确抛错。
- 响应：`Response::text(s)` 为 `text/plain`，默认 `application/json`；
  `json_response(dto)` 流式序列化；`Response::header(name, value)` 链式加头。
- 协议行为由 session/parser 统一处理，不要在 handler 里重复：404、405 + Allow、
  413、百分号解码、keep-alive、pipelining、超时。
- 手工挂载（测试常用）：

```cpp
router r;
r.mount(api);                    // 无切面
r.mount(api, RequestLog{});      // 切面链
Response resp = r.dispatch("GET", "/users/42", "");
```

## 配置（JSON + CLI + CB_ 环境变量）

- 路径：`--config` > `.config(path)` > `application<App>::config_path`。
- 键值优先级：CLI `--key=value` > `CB_` 环境变量 > JSON 文件。
  环境变量映射：`CB_SERVER_PORT` → `server.port`，`_` 转 `.`。
- `main` 用 `App{}.run(argc, argv)` 启用 CLI/环境变量；
  端口优先级 `.port(n) > server.port > default_port > 8080`。
- 命令行示例：`app --config=prod.json --server.port=9000 --db.host=db.prod`。
- 最小形态也可以 `int main() { return run(8080); }`；需要身份/配置载体时再上
  `application<App>`。
- 嵌套对象展平为 `a.b.c`；数组为 `a.0`、`a.1`；`${ENV:default}` 读取时展开。
- 绑定按成员名匹配键尾段；缺失键保留成员默认值；`optional` 缺键为 nullopt；
  `vector` 支持逗号分隔或索引键；非法键会聚合全部错误后一次抛出。
- 构造器标量参数也可用 `[[=Value{"key"}]]` 注入（见 DI 一节），不必写配置结构体。
- Profile 激活列表：`cboot.profiles.active`（逗号分隔，`[[=Profile{"name"}]]` 使用）。
- 服务端键：`server.port`、`server.io`（threads/iocp/epoll）、`server.workers`、
  `server.max_connections`、`server.handler_timeout_ms`、`server.send_timeout_ms`。
- Windows 上选 `epoll` 会明确报错（需 Linux 构建）；`iocp` 仅 Windows。

## JSON

- 支持（绑定目标）：bool、整数、浮点、`std::string`、`optional<U>`、
  `vector<U>`、`std::map<std::string,V>`、`std::unordered_map<std::string,V>`、
  `json::value`、枚举（按名）、聚合体（递归成员）。序列化额外支持
  `std::string_view`；`get_as` 不接受 string_view。超出范围编译错。
- 自定义类型特化 `json::codec<T>`：提供
  `static json::value to_value(const T&)` 与 `static T from_value(const json::value&)`；
  优先于枚举/聚合体默认规则，特化须在使用前可见。
- 热路径用 `json::write_to(out, dto)` 或 `json_response(dto)`：流式写、不建
  value 树。`Response::body` 是 `json::small_buffer<64>`（≤64 字节零堆分配，
  超出自动溢出到 `std::string`），可隐式转 `std::string_view`；赋值 `std::string`
  仍可用，但不要假设它是 `std::string`。
- 需要值模型时用 `json::value` / `parse` / `dump` / `from` / `get_as<T>`；
  `parse` 错误消息带行/列与偏移，嵌套深度默认 128（`parse_options` 可调）。

## 切面（AOP）

- 切面类型提供 `around(const call_info&, Next&&)`；`call_info` 含 `bean`、
  `method`、`point`（`join_point::method` / `post_construct` / `pre_destroy`）。
- 三类切入点：

| 切入点 | 写法 | 切面形态 |
|---|---|---|
| 路由 handler（全局） | `r.mount(obj, As...)` / `App::aspects` | `Response around(...)`，值对象 |
| 路由 handler（按方法） | `[[=web::UseAspect{"name"}]]` | Bean + `[[=web::Aspect{"name"}]]` |
| 生命周期方法 | `App::lifecycle_aspects` / `context(with_lifecycle_aspects, cfg, As...)` | `void around(...)`，值对象 |

```cpp
struct App : application<App> {
    static constexpr auto aspects = std::tuple<RequestLog>{};
    static constexpr auto lifecycle_aspects = std::tuple<LifecycleTrace>{};
};
```

- 方法级选择：切面写成 `[[=Bean{}]] [[=web::Aspect{"tag"}]]`（可注入依赖），
  在 handler 或控制器上 `[[=web::UseAspect{"tag"}]]`。类级对全部路由生效、
  方法级追加、重名只生效一次；层次为全局 → 类级 → 方法级。
- 手工挂载用 `mount_routes(r, ctx, obj, aspects...)`（带注册表）；
  `r.mount(obj)` / `r.mount(obj, As...)` 遇到 `UseAspect` 会编译错。
- 未注册的切面名 → 编译错；切面 Bean 的 profile 未激活 → 挂载期报错。
- 每次 mount 复制一份链实例（全局切面每 bean 一份共享链）；切面里的
  计数器/窗口是跨请求共享的，需要同步时自己加锁。切面 Bean 的实例属于容器，
  天然跨路由共享。
- 生命周期切面是容器构造前的值对象：不能是 Bean、不能注入依赖；
  `PostConstruct` 异常 → 启动失败，`PreDestroy` 异常被隔离并记录。

## 事件总线

- 监听方法用 `[[=EventListener{}]]` 标注，必须 public、非静态、恰一个参数
  （按值或 const 引用）；参数剥 cv/ref 后与事件类型精确相等，不走基类匹配。
- `ctx.publish(event)` 同步派发，顺序为 bean 清单顺序 → 方法声明顺序；
  未激活 Profile 的 bean 跳过。业务 bean 可注入内建依赖 `event_publisher&`
  发布；它不在 bean 清单里，由容器绑定。
- 监听者异常默认写 stderr 并继续；`set_event_error_handler` 可替换，
  返回 `event_result{delivered, failed}`。
- 切面包裹：`ctx.publish(ev, wrap)`，wrap 第三参接 `fn`：
  `chain.run(call_info{bean, method}, fn)`。
- 边界：同步、进程内，无持久化/重试。

## 定时任务（契约组件 cboot.schedule）

- 应用必须 `import cboot.schedule;` 并 `using components =
  type_list<schedule_component>;`；未选择组件时注解不生效。
- 方法用 `[[=Scheduled{1000}]]` / `[[=Scheduled{.fixed_delay_ms = 1000}]]`
  固定延迟；`[[=Scheduled{.expression = "..."}]]` cron 表达式。
- 方法必须 public、非静态、无参数；`fixed_delay_ms` 与 `expression` 二选一。
- cron 支持 5 段（分 时 日 月 周）和 6 段（秒 分 时 日 月 周）；
  `*`/`?`、范围、列表、步长；按本地时区；非法表达式编译期报错。
- 单调度线程串行执行；任务异常写 stderr 后继续；hook 析构停止线程；
  Profile 未激活不注册。
- 任务应短小；当前无任务线程池。

## ORM（契约组件 cboot.orm + 驱动）

- 分层：`cboot.orm` 只含 annotations/driver/meta/dsl/EntityManager；
  `cboot.orm.postgres` 与 `cboot.orm.mysql` 各自提供配置、dialect、
  DataSource 和驱动实现。通用层不得 include libpq/mysql.h。
- 应用选择驱动组件：
  `import cboot.orm.postgres;`（或 `cboot.orm.mysql`），
  `using components = type_list<postgres_component>;`；
  `postgres_component` 的 `dependencies = type_list<orm_component>`。
- 实体注解：`[[=orm::Table{.name, .schema}]]`、`[[=orm::Column{"..."}]]`、
  `[[=orm::Id{.generated=true}]]`；实体是 aggregate，恰好一个 Id。
- `EntityManager`：`find/all/insert/update/remove/create_schema/drop_schema`；
  自增主键按 dialect 选择 `RETURNING` 或 `last_insert_id`。
- 事务：`db.transaction([](orm::session& tx) { ... })`；固定一个连接，
  正常提交、异常回滚；session 提供同样的 CRUD/DSL/原生 SQL。
- 原生 SQL：`fetch_rows` 按 SELECT 列顺序映射；`query<T>` 按结果列名
  映射，支持 alias/join；驱动 statement 提供 `column_count/column_name`。
- DSL：`db.select<T>().where(...).order_by(...).limit()/offset().fetch()`；
  `col<^^T::field>()`，支持比较、`is_null`、`in`、`like/ilike`、`&&/||/!`。
  条件保存 AST，执行时才按 `dialect` 渲染。
- 配置前缀 `db`：PostgreSQL 用 `PgProps`（url/user/password/sslmode/池），
  MySQL 用 `MysqlProps`（host/port/database/user/password/charset/池）。
  password 可留空或写 `${PGPASSWORD}` / `${MYSQL_PWD}`。
- 第三方头只进实现单元：PG 用 `PQexecParams`；MySQL 用
  `mysql_real_escape_string` 的文本协议（不是服务端预处理语句）。
- CMake：`cboot::orm_postgres` / `cboot::orm_mysql`；包名
  `cboot.orm.postgres` / `cboot.orm.mysql`；MySQL 用 SDK 的
  `FindMySQL.cmake`，可传 `MYSQL_ROOT`/`MySQL_ROOT`。
- Windows 运行时需要 `libpq.dll` / `libmysql.dll` 在 PATH 或 exe 同目录；
  MySQL 测试 target 会复制 DLL。
- 集成测试：`CBOOT_TEST_PG_URL`、`CBOOT_TEST_MYSQL_DATABASE`（及可选
  `CBOOT_TEST_MYSQL_HOST/PORT/USER/PASSWORD`）；未设置时只跑单元测试。
- repository 模板尚未接入；事务为单层，不支持嵌套/savepoint。

## DTO 校验（基础包 cboot.validation）

- 不是契约组件，不参与 `components` 选择；`import cboot.app` 已 re-export。
- 字段注解：`[[=validation::NotBlank{}]]`、`[[=validation::Range{.min,.max}]]`；
  可带 `message` 自定义文案。
- `validation::validate(dto)` 返回 `std::vector<field_error>`；
  `validate_or_throw(dto)` 失败抛 `validation::invalid`。
- HTTP body DTO 绑定后自动校验；失败与 JSON 语法/类型错误统一 400：
  `{"errors":[{"field":"...","message":"..."}]}`。
- 只支持顶层字段；NotBlank 限 `std::string`，Range 限整数/浮点；
  不做嵌套递归、自定义 validator、正则或国际化。
- 实现位置：`packages/validation/validation.cppm`；web 映射在
  `packages/web/routing.cppm` / `src/routing.cpp`。
- 测试：`validation_smoke`、`web_routing_smoke`。

## 中间件与认证（web middleware / cboot.jwt / cboot.auth）

- 扩展点固定四类：`beans`、`hooks`、`middleware`、`aspects`
  （路由 `aspects` + 生命周期 `lifecycle_aspects` 两份清单）。
  middleware 是 Bean 角色，不新增组件描述符字段。
- 中间件 Bean：`[[=web::Middleware{.name, .order}]]` + `handle(request_context&, next_fn)`。
  `name` 为空 = 全局；非空 = 仅路由带 `[[=web::UseMiddleware{"name"}]]` 时执行。
- `Request` 暴露原始 headers 与 `header("Authorization")`；`http_method`
  包含 `OPTIONS`。`request_context` 有类型化属性槽（`put/find/get`）；
  `get<T>()` 在属性缺失时抛 `logic_error`（fail-loud）。
- handler 可声明 `const web::request_context&` 参数；顺序：
  parser → middleware → route → aspects → handler。
- 全局错误层：`web::ErrorHandler` 是普通全局 middleware（order -900，
  CORS 内层），捕获下游异常并接管 router 标记的 404/405/校验失败；
  应用用 `[[=Bean{}]] [[=web::ErrorMapper{.order=N}]]` +
  `handle(error_info, ctx) -> optional<Response>` 自定义响应。
  AuthGuard 抛 `web::error`，不再自己拼 401/403。解析 400/413 与
  504 属 connector/SLA 层，不经过 ErrorHandler。
- CORS：可选 `cors_component`（`cboot::web`）贡献 `web::CorsProps` +
  全局 `web::Cors`（order -1000）。来源白名单支持 `*` 通配；预检
  `OPTIONS` 204 短路，校验方法/请求头；`allow_credentials` 禁止裸 `*`。
  不选组件则无 CORS 行为。
- `cboot.jwt`：`JwtProps`（前缀 `jwt`）、`JwtVerifier`；HS256/RS256，
  exp/nbf/iss/aud，OpenSSL 只在实现单元 include。测试/运行时需 OpenSSL。
- `cboot.auth`：登录态 starter，不依赖 jwt。核心组件 `auth_component`
  （AuthProps/AuthPolicy/AuthService/AuthGuard + policy hook）不直接提供
  TokenStore；应用选 `auth_memory_component` 或 `auth_postgres_component`。
- `AuthGuard` 按 `public_paths` / `protected_paths` / `default_policy` 匹配；
  成功写入 `auth::session`，失败发布 `authentication_*` /
  `authorization_failed` 并抛 `web::error`（401/403），由 ErrorHandler 生成响应。
- token 传输：`token_header`/`token_prefix`，`token_cookie` 非空时 header
  缺失回退 Cookie；`attach_cookie` / `clear_cookie` 写 Set-Cookie。
- 滑动续期：`active_timeout_s > 0` 时每次认证把 expires_at 延到
  `now + active_timeout_s`，上限 `issued_at + timeout_s`；store 覆盖
  `touch(token, expires_at)` 才落库（memory/PG 已实现）。
- RBAC：`[[=auth::RequireRole{"admin"}]]` / `[[=auth::RequireScope{"..."}]]`
  可标在 handler 或控制器类型；类级要求对全部路由生效，方法级追加。
  `auth_policy_hook` 启动时合并类级+方法级要求填充 AuthPolicy，AuthGuard
  查表返回 403。web 不依赖 auth。
- `cboot.auth.postgres`：`PgTokenStore`（表 `cboot_auth_sessions`，
  含 `issued_at`，PostConstruct 自动补列），替代 Redis/内存 store；
  依赖 `auth_component` + `postgres_component`。
- CMake：`cboot::auth` / `cboot::auth_postgres` / `cboot::jwt`；
  auth 不依赖 OpenSSL，jwt 依赖 OpenSSL。
- 测试：`web_middleware_smoke`、`cors_smoke`、`error_smoke`、`log_smoke`、
  `jwt_smoke`、`auth_smoke`、`auth_postgres_smoke`（需 `CBOOT_TEST_PG_URL`）。

## 日志（cboot.log）

- 模块 `cboot.log`，命名空间 `logging`（全局 `log` 与 C `math.h` 的
  `double log(double)` 冲突，不能叫 `namespace log`）。
- 基础包默认 console + info 始终可用；`log_component` 读取 `log.*`：
  `level` / `format` / `pattern` / `output` / `file` / `max_size_mb` /
  `max_files` / `file_level` / `logger_levels`。
- API：`logging::get("name")` → `logger`；`trace/debug/info/warn/error`
  用 `std::format`；启动期 `set_level` / `set_layout` / `add_sink` /
  `clear_sinks` / `set_logger_level` / `flush`。
- 内置 `console_sink` / `file_sink` / `rolling_file_sink`；text pattern
  支持 `%d %level %logger %msg %thread %n %X{key}`；JSON Lines。
- MDC：`logging::context::scope ctx("request_id", id)`。
- `web` 的 `ErrorHandler` 5xx 记 error、4xx 记 debug；app 启动/停止记 info。
- 测试：`log_smoke`（级别过滤、前缀覆盖、JSON/MDC、file/rolling、LogSetup）。

## 健康检查

- 应用自动挂载 `GET /health`：JSON `status/beans/startup_ms`，DOWN 时 503 + reason。
- 业务 bean 注入内建 `health_registry&`，用 `set_unhealthy("...")` /
  `set_healthy()` 标记；它不在 bean 清单里。
- `GET /beans` 仅 `dev` profile 暴露，返回 type/name/profile/active/deps。

## 协程

- `task<T>` 惰性启动、对称转移、异常经 promise 传播；`sync_wait` 只能等待
  不挂起在外部事件上的任务。
- `awaitable_event` 一次性；`async_event` auto-reset，供 I/O 完成唤醒。
- IOCP 连接处理内部由协程驱动；应用层通常只需返回 `task<Response>`。
- 当前没有自带调度器；不要假设有线程池可 `co_await`。

## 测试与新增功能

- 冒烟测试按类别放 `test/<category>/*_smoke.cpp`
  （meta/core/app/schedule/json/config/aop/async/web）：`import cboot...`，
  失败用不同返回码，`main` 返回非 0 即失败。
- 网络测试用临时端口（`server(0)` / `server.port = 0`）并轮询 `wait_status`，
  不要固定 sleep；IOCP 用例要与 threads 后端各跑一遍。
- 负面用例放 `test/negative/negative_*.cpp`，在 CMake 里注册为“预期编译失败”；
  改错误信息时同步更新它们的注释。
- 跨包回归测试在根 `test/`；包内测试用
  `cboot_package(... TEST_SOURCES test/<pkg>_smoke.cpp)`，测试目标会自动
  直接链接本包 `DEPENDS` 的 provider target。
- 改公开 API 后同时更新 `README.md`、`docs/使用手册.md`、
  `docs/契约组件.md`、`docs/定时任务.md`、`docs/ORM.md`、`docs/AOP.md`、
  `docs/认证.md`、`docs/CORS.md`、`docs/错误处理.md` 与 `docs/日志.md`；
  `docs/任务清单.md` 是本地忽略文件，按需更新。

## GCC 16 反射/模块排错清单

- 注解没生效：先查 `{}`、位置（class-key 之后）、值类型是否 structural。
- `'export' may only occur once`：已经在 `export namespace` 里，去掉内层 `export`。
- 拼接（splice）不能直接作模板实参：用 `type_of_t<info>` 桥接。
- `template for` 不能直接遍历 `vector<info>`（常量表达式失败）：用
  `members_array<R>()` 适配。
- 全局模块片段（GMF）不跨单元共享：impl 单元若用 winsock，需重复守卫宏与
  `#include <winsock2.h>`，不能依赖接口单元的 GMF。
- 模块内提前实例化导致扫描为 0：扫描标签必须依赖调用方类型（`application<App>`
  或 `std::tuple<As...>`），不要写死成非依赖特化。
- 导出模板引用了非导出 helper：消费方链接会 undefined reference；把 helper
  放进接口单元并导出，或在模板内联实现。
- `find_package(cboot)` 消费方：模块 provider 目标要直接链接（`cboot::app`），
  旧 `cboot::cboot_app` 已不存在；`-freflection` 要目录级生效，
  `cxx_std_26` 要显式声明。
- 包内测试直接 import 依赖模块时，测试目标也要直接链接对应 provider；
  `cboot_package_target()` 已把 `DEPENDS` 一并挂到测试目标上。
- 模板实参里不能嵌 consteval 调用（`f<T, I>(k)` 的 `>` 提前闭合，GCC16 报
  "parse error in template argument list" / "cannot resolve overloaded function"）：
  下标改成模板参数，或把反射存进类里的 `static constexpr` 再取。
- 按 N 递归的 consteval 取下标器要 `if constexpr (N == 0)` 守卫：零长度
  `std::array` 的 `operator[]` 非法（空 bean 清单即触发）。
- 持引用元素的 `std::tuple` 构造用 `std::forward<As>(as)...`；`std::move`
  会把左值引用变成亡值，无法绑定到 `T&` 元素（`aspect_chain<Tag&>` 踩过）。

## 代码注释（必须遵守）

- **所有新增/修改的代码都必须有中文注释**：`.cppm` / `.cpp` / CMake / 测试文件都不例外。
- **每个代码文件顶部必须有文件级文档块**，使用固定风格：

```cpp
/**
 * @file orm.dsl.cppm
 * @brief ORM 类型安全 DSL
 *
 * 提供单表 select/update/delete 的类型安全构造；
 * 不提供 join/聚合/CTE，复杂 SQL 走原生逃生口。
 */
```

- 公开类型、函数、builder、CMake 函数都用 `@brief` 文档块，说明能力、边界、
  参数语义和生命周期；不要只写一行 `//` 注释。
- 注释解释“为什么、约束、边界、生命周期”，不要复述代码字面。
- 模板、反射、splice、模块边界、导出规则必须写清限制和原因。
- CMake target / 导出 / 安装规则要有注释；组件文件头说明贡献什么。
- 测试文件头写覆盖点；负面测试注明预期诊断。
- 不写无意义注释；但“没有文件级/接口级注释”视为不合规。

## 协作约定

- 注解名保持扁平裸名（`Bean`/`Inject`/`Get`），工具 API 只用一级命名空间
  （`json::`、`config::`）；不要引入 `cboot::` 前缀。
- 注释用中文、解释“为什么”，与周边代码密度一致。
- 文件修改用直接编辑工具，不用脚本批量生成/改写；用户需要看到具体 diff。
- CHANGELOG 只记录**已实现的框架/构建代码变更**；待实现项、文档修改、
  示例修改不写入日志。条目头用 `# <version> — <date>`，与既有格式一致。
- 不要新增角色注解、不要恢复隐式控制器注册、不要把编译期装配改成运行期扫描。
