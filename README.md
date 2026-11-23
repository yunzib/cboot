<div align="center">
<h1>cboot-framework（C++26 反射应用框架）</h1>
<p>注解驱动 · 编译期装配 · 契约组件 · 轻量 HTTP 服务</p>
</div>

<p align="center">
<img alt="" src="https://img.shields.io/badge/release-v0.1.0-brightgreen" style="display: inline-block;" />
<img alt="" src="https://img.shields.io/badge/c++-26-brightgreen" style="display: inline-block;" />
<img alt="" src="https://img.shields.io/badge/gcc-16.2-brightgreen" style="display: inline-block;" />
<img alt="" src="https://img.shields.io/badge/cmake-4.3-brightgreen" style="display: inline-block;" />
<img alt="" src="https://img.shields.io/badge/license-MIT-blue" style="display: inline-block;" />
</p>

# 1. 项目描述

- **cboot-framework** 是基于 C++26 静态反射（P2996）的应用框架：依赖装配、路由注册、配置绑定在**编译期**完成，运行时只做 HTTP I/O 与序列化。
- 极简注解内核：`Bean` / `Inject` / `Named` / `Value` / 生命周期键 + 路由方法注解；不设 Service/Controller 等角色注解。
- **编译期 DI**：构造器注入、拓扑排序；同基类多实例用 `[[=Named{"..."}]]` 选择；
  环依赖、缺失依赖、构造器歧义直接编译错。
- **注解即注册**：`[[=Bean{}]]` 自动发现，零清单启动；控制器漏标直接编译错并指出类型名。
- **REST**：Get/Post/Put/Delete/Patch、路径参数、JSON body、405 + Allow、413、keep-alive、pipelining。
- **配置**：JSON 文件 + `${ENV:default}`；`[[=Config{"prefix"}]]` 通用配置 bean；
  `[[=Value{"key"}]]` 直接注入标量；`[[=Profile{"dev"}]]` 按 profile 条件装配。
- **JSON**：反射流式序列化，不建 value 树；支持 map/unordered_map、
  `json::codec<T>` 自定义类型；≤64 字节响应零堆分配。
- **DTO 校验**：`[[=validation::NotBlank{}]]` / `[[=validation::Range{...}]]`，
  body 绑定自动校验，失败返回 400 + 字段错误列表。
- **middleware**：Bean 角色 + 全局/路由级链，支持 `Request` headers、
  `request_context`、短路、CORS/OPTIONS；中间件失败不会进入 handler。
- **认证**：`cboot.auth` 登录态（opaque token、login/logout/kickout、
  路由 public/protected 策略、认证事件）；`cboot.jwt` 独立提供 HS256/RS256 校验。
- **AOP**：编译期切面链，零代理、零虚调用。
- **事件**：`[[=EventListener{}]]` + `ctx.publish` / 注入 `event_publisher`，
  同步派发、顺序稳定、异常隔离，可接 `aspect_chain`。
- **契约组件**：`components` / `dependencies` 编译期装配，组件可贡献 bean 与 hooks；
  首个组件 `cboot.schedule`。
- **定时**：`[[=Scheduled{...}]]` 固定延迟 / 5/6 段 cron，单调度线程、异常隔离、停机取消。
- **ORM（可选组件）**：通用类型安全 ORM + PostgreSQL / MySQL 驱动，
  实体元数据、CRUD、查询 DSL、事务、原生 SQL 列名映射。
- **运维**：内建 `GET /health`（200/503 + JSON）、`GET /beans`（仅 dev）。
- **传输**：threads / IOCP / epoll；万级连接、背压 503、handler 超时 504、优雅停机。
- **协程**：`task<T>` / `sync_wait`；handler 可返回 `task<Response>`。
- **工程**：C++20 命名模块 + `import std` + CMake 包（`find_package(cboot)`）。

注解使用扁平裸名（`Bean` / `Inject` / `Named` / `Value` / `Get` / `Response`），无 `cboot::` 前缀；
`struct` 与 `class` 均支持，`class` 的构造器、handler、生命周期方法须 public，
继承 `application` 时须写 `public`。

# 2. 开发环境

| 项 | 值 |
|---|---|
| 操作系统 | Windows 10/11（IOCP）；Linux（epoll，待 CI 验证） |
| 编译器 | GCC 16.2.0（niXman x86_64-16.2.0-release-posix-seh-ucrt-rt_v14-rev1，`-freflection`） |
| 构建系统 | CMake 4.3.2 + Ninja |
| 语言标准 | C++26 + C++20 命名模块 + `import std` |
| 反射能力 | P2996R13 / P3394R4 / P1306（GCC 16 实测） |
| 其他编译器 | Clang/MSVC 暂无 P2996 实现，不提供构建口径 |

# 3. 代码示例

## 3.1 最小应用

```cpp
import cboot.app;
import std;

struct UserDto { std::string name; int age{}; };

class [[=Bean{"repo"}]] UserRepo {
public:
    UserRepo() = default;     // 注入前提：public 构造器
    UserDto find(int id) {
        return {.name = "user" + std::to_string(id), .age = 20 + id};
    }
};

class [[=Bean{}]] UserApi {
public:
    UserApi(UserRepo& repo) : repo_(repo) {}

    [[=Get{"/users/{id}"}]]
    Response get(int id) { return json_response(repo_.find(id)); }

    [[=Post{"/users"}]]
    Response create(const UserDto& in) { return json_response(in); }

private:
    UserRepo& repo_;          // 私有状态允许；框架不访问
};

class App : public application<App> {   // class 继承须写 public
public:
    App() = default;
    static constexpr unsigned short default_port = 8080;
};

int main() { return App{}.run(); }
```

`class` 需注意：构造器、handler、生命周期方法必须 public；继承 `application<App>`
要显式 `public`。DTO 保持聚合体（`struct`，或成员全 public 且无用户构造器的
`class`），否则 JSON body 绑定编译错。

`App` 是自动发现的扫描锚点：当前 TU 可见、标注 `[[=Bean{}]]` /
`[[=Config{...}]]` 的完整类型自动纳入，无需注册清单。

## 3.2 请求与响应

```bash
curl http://127.0.0.1:8080/users/42
curl -X POST -d '{"name":"bob","age":30}' http://127.0.0.1:8080/users
```

handler 参数按声明序解释：

| 参数类型 | 来源 |
|---|---|
| `Request` | 请求本体（method / path / query / body 均为视图） |
| 聚合体（`struct` 或全 public `class`） | JSON body 反序列化 |
| 数值 / 字符串 | 路径参数（按 `{x}` 出现序消费） |

返回值为 `Response`；也可返回 `task<Response>`。路由表与参数绑定在编译期生成，
运行时只做路径匹配与派发。

## 3.3 配置

```json
{
  "server": { "port": 8080, "io": "iocp", "workers": 8 },
  "db": { "host": "${DB_HOST:localhost}", "port": 5432 }
}
```

```cpp
struct [[=Config{"db"}]] DbProps {
    std::string host{"localhost"};
    int port{5432};
};

struct App : application<App> {          // 在 3.1 的 App 上增加配置路径
    static constexpr std::string_view config_path = "app.json";
};
```

端口优先级：`.port(n)` > `server.port` > `default_port` > 8080。
其他键：`server.io`（threads/iocp/epoll）、`server.workers`、`server.max_connections`、
`server.handler_timeout_ms`、`server.send_timeout_ms`。

# 4. 运行与安装

## 4.1 构建

工具链路径已固化在 `CMakePresets.json`。Ninja 为单配置生成器，Debug/Release 分树：

```bash
cmake --preset gcc                  # Debug 树 → build/
cmake --build --preset gcc

cmake --preset gcc-release          # Release 树 → build-release/
cmake --build --preset gcc-release
```

## 4.2 测试

```bash
ctest --preset gcc                  # 31 项（MySQL + OpenSSL 启用时；裁剪后相应减少）
ctest --test-dir build-release --output-on-failure
```

负面用例（环依赖、缺失依赖、构造器歧义、控制器漏标 Bean）是**预期编译失败**，
用于锁定诊断文案。

## 4.3 安装与外部工程

```bash
cmake --install build-release --prefix <prefix>
```

```cmake
find_package(cboot REQUIRED)
# 模块 provider 必须直接链接
target_link_libraries(app PRIVATE cboot::app)
```

消费方需 `CMAKE_CXX_STANDARD 26`、`CMAKE_CXX_MODULE_STD ON`、
`CMAKE_CXX_SCAN_FOR_MODULES ON`、`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD=<uuid>`；
`cbootConfig.cmake` 会注入 `-freflection` 与运行时链接项。

独立应用示例（Release 安装 + `find_package` + class 风格）见
[examples/standalone/README.md](examples/standalone/README.md)。

## 4.4 性能

Release 构建、8 连接、16 万请求、0 失败：

| 场景 | 1 worker | 8 workers |
|---|---|---|
| 流水线 plaintext（depth=64） | **1.25M** req/s | **2.11M** req/s |
| 流水线 JSON（depth=64） | **0.94M** req/s | **2.07M** req/s |
| 顺序 plaintext（depth=1） | 45.7k req/s | 71.9k req/s |
| 顺序 JSON（depth=1） | 44.5k req/s | 81.6k req/s |
| 并发连接（8 workers） | — | 10,000 / 10,000 全部服务 |
| 微基准（单线程） | 派发 112 ns / 会话 316 ns | — |

同等工作量的 Java 对照：cboot-framework 每核 **+13%（plaintext）/ +17%（JSON）**。
完整口径、瓶颈分析与复现命令见 [docs/性能测试.md](docs/性能测试.md)。

# 5. 文档

- [使用手册](docs/使用手册.md)
- [AOP 切面](docs/AOP.md)
- [事件总线](docs/EventBus.md)
- [契约组件](docs/契约组件.md)
- [定时任务](docs/定时任务.md)
- [DTO 校验](docs/校验.md)
- [中间件](docs/中间件.md)
- [自定义认证](docs/自定义认证.md)
- [认证](docs/认证.md)
- [JWT](docs/JWT.md)
- [ORM 组件](docs/ORM.md)
- [更新日志](docs/CHANGELOG.md)
- [性能测试与 Java 对拍](docs/性能测试.md)
- [完整示例（独立应用：安装 SDK + find_package + class）](examples/standalone/README.md)
- [ORM 示例（PostgreSQL + find_package(cboot.orm.postgres)）](examples/orm/README.md)

# 6. 目录

```
packages/  同级包：meta/json/config/core/aop/async/validation/web/app
           契约组件 schedule / orm + 驱动 / jwt / auth
cmake/     包构建 helper（cboot_package / cboot_component）
test/      按类别归类的冒烟测试；negative/ 负面用例、bench/ 压测
examples/  使用示例
```

# 7. 许可

MIT License，见 [LICENSE](LICENSE)。
