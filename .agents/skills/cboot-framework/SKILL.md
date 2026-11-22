---
name: cboot-framework
description: 在 cboot 仓库里开发或修改基于 C++26 静态反射的应用框架代码。当用户要新增 Bean、控制器、REST 路由、配置绑定、切面、协程 handler，排查编译期依赖注入/注解/模块错误，或改动 modules/、src/、examples/ 时使用；包含最小应用、注解与 DI 规则、路由参数协议、配置键、构建测试命令，以及 GCC 16 反射/模块的已知约束。
---

# cboot 框架开发

cboot 把 Spring 的运行时装配搬到编译期：注解在编译期被反射读取，依赖图、路由表、
配置绑定在启动前确定；运行时只做 I/O、解析和序列化。改动时优先保持这个边界，
不要把注册/扫描/装配搬到运行期。

## 仓库与工具链

- 主工具链只有 GCC 16.2（niXman MinGW），C++26 + `-freflection` + `import std`；
  Clang/MSVC 没有 P2996 实现，不要为它们加兼容分支。
- 接口单元在 `modules/**/*.cppm`（`export module`），实现单元在 `src/**/*.cpp`；
  `test/` 只放测试，`examples/` 只放使用示例。
- 模块分层：`app → web.server → routing/http → core/json/aop → meta`。
  新增依赖时沿这个方向，不要反向 import。
- 单入口：`import cboot.app;` 已 re-export 全部框架词汇（Bean/Inject/路由/
  Request/Response/json_response/application/...）。示例里也可以按需 import 子模块。

构建与测试：

```bash
cmake --preset gcc && cmake --build --preset gcc && ctest --preset gcc
cmake --preset gcc-release && cmake --build --preset gcc-release
ctest --test-dir build-release --output-on-failure
```

改动框架后 Debug/Release 都要过 21 项 ctest；负面用例（`negative_*.cpp`）是
预期编译失败，不能“修好”。

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
| `[[=Scheduled{ms}]]` | 方法 | 固定延迟定时任务（public/非静态/无参数） |
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

## 配置（JSON，替代 YAML）

- 路径：`application<App>::config_path` 静态指定，或 `.config(path)` 运行时覆盖；
  端口优先级 `.port(n) > server.port > default_port > 8080`。
  链式覆盖示例：`App{}.port(9000).config("prod.json").workers(8).run();`
- 最小形态也可以 `int main() { return run(8080); }`；需要身份/配置载体时再上
  `application<App>`。
- 嵌套对象展平为 `a.b.c`；数组为 `a.0`、`a.1`；`${ENV:default}` 读取时展开。
- 绑定按成员名匹配键尾段；缺失键保留成员默认值；`optional` 缺键为 nullopt；
  `vector` 支持逗号分隔或索引键。
- 构造器标量参数也可用 `[[=Value{"key"}]]` 注入（见 DI 一节），不必写配置结构体。
- Profile 激活列表：`cboot.profiles.active`（逗号分隔，`[[=Profile{"name"}]]` 使用）。
- 服务端键：`server.port`、`server.io`（threads/iocp/epoll）、`server.workers`、
  `server.max_connections`、`server.handler_timeout_ms`、`server.send_timeout_ms`。
- Windows 上选 `epoll` 会明确报错（需 Linux 构建）；`iocp` 仅 Windows。

## JSON

- 支持（绑定目标）：bool、整数、浮点、`std::string`、`optional<U>`、
  `vector<U>`、枚举（按名）、聚合体（递归成员）。序列化额外支持
  `std::string_view`；`get_as` 不接受 string_view。超出范围编译错。
- 热路径用 `json::write_to(out, dto)` 或 `json_response(dto)`：流式写、不建
  value 树。`Response::body` 是 `json::small_buffer<64>`（≤64 字节零堆分配，
  超出自动溢出到 `std::string`），可隐式转 `std::string_view`；赋值 `std::string`
  仍可用，但不要假设它是 `std::string`。
- 需要值模型时用 `json::value` / `parse` / `dump` / `from` / `get_as<T>`。

## 切面（AOP）

- 切面类型提供 `around(const call_info&, Next&&)`；`call_info` 含 `bean`、`method`。
- `r.mount(obj, As...)` 或 `App::aspects = std::tuple<As...>{}` 编译期织入，
  无运行时代理：

```cpp
struct App : application<App> {
    static constexpr auto aspects = std::tuple<RequestLog>{};
};
```
- 每次 mount 复制一份链实例；切面里的计数器/窗口是跨请求共享的，需要同步时
  自己加锁。

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

## 定时任务

- 方法用 `[[=Scheduled{1000}]]` / `[[=Scheduled{.fixed_delay_ms = 1000}]]` 标注；
  public、非静态、无参数、delay > 0。
- 单调度线程，固定延迟（执行完成后等待 delay）；首次在 delay 之后。
- 任务异常写 stderr 后继续；容器析构 `request_stop` + `join`；Profile 未激活不注册。
- 任务应短小；当前无 cron / 线程池。

## 协程

- `task<T>` 惰性启动、对称转移、异常经 promise 传播；`sync_wait` 只能等待
  不挂起在外部事件上的任务。
- `awaitable_event` 一次性；`async_event` auto-reset，供 I/O 完成唤醒。
- IOCP 连接处理内部由协程驱动；应用层通常只需返回 `task<Response>`。
- 当前没有自带调度器；不要假设有线程池可 `co_await`。

## 测试与新增功能

- 冒烟测试在 `test/*_smoke.cpp`：`import cboot...`，失败用不同返回码，
  `main` 返回非 0 即失败。新增路由/绑定/DI 行为时照此加用例。
- 网络测试用临时端口（`server(0)` / `server.port = 0`）并轮询 `wait_status`，
  不要固定 sleep；IOCP 用例要与 threads 后端各跑一遍。
- 负面用例放 `test/negative_*.cpp`，在 CMake 里注册为“预期编译失败”；
  改错误信息时同步更新它们的注释。
- 改公开 API 后同时更新 `README.md` 与 `docs/任务清单.md` 的用法/状态。

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
- `find_package(cboot)` 消费方：模块 provider 目标要直接链接，`-freflection`
  要目录级生效，`cxx_std_26` 要显式声明。

## 协作约定

- 注解名保持扁平裸名（`Bean`/`Inject`/`Get`），工具 API 只用一级命名空间
  （`json::`、`config::`）；不要引入 `cboot::` 前缀。
- 注释用中文、解释“为什么”，与周边代码密度一致。
- 文件修改用直接编辑工具，不用脚本批量生成/改写；用户需要看到具体 diff。
- 不要新增角色注解、不要恢复隐式控制器注册、不要把编译期装配改成运行期扫描。
