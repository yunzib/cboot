# AOP 切面

cboot-framework 的 AOP 是**编译期展开的静态调用链**：没有运行时代理、没有虚调用、没有每次请求的链分配。
切入点有三类：路由 handler（全局或按方法选择）、生命周期方法（`PostConstruct` / `PreDestroy`）、
事件监听者（`ctx.publish(event, wrap)` 显式接入同一条链）。

相关文件：

| 文件 | 内容 |
|---|---|
| `packages/aop/aop.cppm` | `call_info` / `join_point` / `AspectFor` / `aspect_chain` / `around` |
| `packages/web/routing.cppm` | `router::mount`、`web::Aspect` / `web::UseAspect`、handler 包装 |
| `packages/core/context.cppm` | 生命周期切面（`with_lifecycle_aspects`） |
| `packages/app/app.cppm` | 切面注册表（名称 → 切面 Bean）、`App::aspects` / `App::lifecycle_aspects` |
| `test/aop/aop_smoke.cpp` | 洋葱顺序、短路、异常、非 web、概念约束 |
| `test/aop/method_aspect_smoke.cpp` | 方法级 / 控制器级选择、去重、层次、切面 Bean 注入 |
| `test/aop/lifecycle_aspect_smoke.cpp` | 生命周期拦截、短路、失败语义 |
| `test/negative/negative_use_aspect_*.cpp` | 未注册名 / 无注册表 mount 的编译错 |
| `examples/standalone/main.cpp` | `RequestLog` 示例 |

---

## 1. 设计目标

- 切面是普通结构体，不需要继承接口或注册类型
- 链在 `mount` 时建立一次，请求期不构造、不分配
- 切面调用由模板展开成直接调用，优化器可以内联
- 切面对象可以持有状态（计数器、限流窗口、日志缓冲）
- 只包装框架调用的位置，不做隐式代理

---

## 2. 切入点

| 切入点 | 挂载方式 | 范围 | 详见 |
|---|---|---|---|
| 路由 handler | `mount(obj, aspects...)` / `App::aspects` | 全局：该 bean（应用）的全部路由 | 2、5.1、5.2 |
| 路由 handler | `[[=web::UseAspect{"name"}]]` | 方法级：只作用于标注的路由 | 5.5 |
| `PostConstruct` / `PreDestroy` | `context(with_lifecycle_aspects, ...)` / `App::lifecycle_aspects` | 全部 bean 的生命周期方法 | 5.6 |
| 事件监听者 | `ctx.publish(event, wrap)` | 显式接链 | 5.4 |

`router::mount(obj, aspects...)` 为 bean 的每个路由 handler 包一层链：

```cpp
auto chain = std::make_shared<aspect_chain<As...>>(std::move(aspects)...);
mount_with_chain<T, 0>(obj, std::move(chain));
```

`mount_with_chain` 递归扫描路由方法，把 handler 注册为 lambda：

```cpp
[&obj, chain](const Request& req, const std::vector<std::string_view>& pp) {
    return chain->run(call_info_for<T, I>(),
                      [&]() -> Response {
                          return invoke<T, nth_handler<T, I>()>(obj, req, pp);
                      });
}
```

请求期路径：

```
dispatch
  → std::function<Response(const Request&, const vector<string_view>&)>
    → chain->run(call_info, handler_lambda)
      → aspect[0].around(...)
        → aspect[1].around(...)
          → invoke<T, Fn>(obj, req, pp)
```

没有切面的 `mount(obj)` 直接注册 `invoke`，不创建链。

用了方法级选择的 bean 是两层链（见 5.5）：外层为全局切面（每个 bean 一条共享链），
内层为该路由按 `UseAspect` 选出的切面 Bean（每条路由一条），最里面才是 handler。

---

## 3. 工作原理

### 3.1 链的展开

`aspect_chain<As...>::run` 用递归模板逐层包裹：

```cpp
template <std::size_t I, class R, class Core>
R run_at(const call_info& ci, Core& core) {
    if constexpr (I == sizeof...(As)) {
        return core();                    // 最内层：handler
    } else {
        auto& aspect = std::get<I>(aspects_);
        return aspect.around(ci, [&]() -> R {
            return run_at<I + 1, R>(ci, core);
        });
    }
}
```

`mount(api, Trace{}, Auth{})` 展开后等价于：

```cpp
Trace::around(ci, [&] {
    return Auth::around(ci, [&] {
        return handler(req, pp);
    });
});
```

洋葱顺序：`As[0]` 最外层，先进入、最后退出。

### 3.2 call_info

`call_info{bean, method}` 在编译期由反射生成：

```cpp
consteval call_info call_info_for() {
    return call_info{ identifier_of(^^T), identifier_of(nth_handler<T, I>()) };
}
```

两个字段都是 `std::string_view`，指向编译器生成的标识符存储：

- 改类名/方法名后自动更新，不需要手写字符串常量
- 请求期零字符串构造

第三个字段 `point` 标明连接点，生命周期切面靠它区分两个阶段：

```cpp
enum class join_point : std::uint8_t {
    method,          // 路由 handler / 事件监听 / 手工链（默认值）
    post_construct,  // [[=PostConstruct{}]]
    pre_destroy,     // [[=PreDestroy{}]]
};
```

手工链和路由链不填 `point`，保持 `method`。

### 3.3 运行时边界

路由表里存的是 `std::function<Response(const Request&, const vector<string_view>&)>`。
每请求有一次类型擦除调用（`std::function`），进入 lambda 后：

- 链遍历是模板实例化的直接调用
- `around` / `next()` 可内联
- 切面对象在 `mount` 时复制进 `std::tuple`，请求期只读/改成员
- 链本身由 `shared_ptr` 持有，只在该 bean 的 mount 时分配一次

---

## 4. 编写切面

### 4.1 契约

```cpp
struct MyAspect {
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        // ...
        return next();
    }
};
```

- 普通结构体，无继承、无虚函数
- `around` 是模板，`Next` 是 continuation
- `next()` 返回类型由 handler 决定；web 场景为 `Response`
- 非 web 场景可用 `auto` 返回，由 `next()` 推导
- 生命周期方法返回 `void`，切面写成 `void around(const call_info&, Next&& next)`，`next()` 不返回值（见 5.6）

### 4.2 常见写法

**耗时日志**

```cpp
struct RequestLog {
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        auto t0 = std::chrono::steady_clock::now();
        Response r = next();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::println("[aop] {}.{} -> {} ({} us)",
                     ci.bean, ci.method, r.status, us);
        return r;
    }
};
```

**鉴权短路**

```cpp
struct Auth {
    bool allow = false;
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        if (!allow) {
            Response r = Response::text("forbidden");
            r.status = 403;
            return r;                  // 不调用 next()
        }
        return next();
    }
};
```

**异常兜底**

```cpp
struct CatchAll {
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        try {
            return next();
        } catch (const std::exception& e) {
            std::println("[aop] {}.{} failed: {}", ci.bean, ci.method, e.what());
            Response r = Response::text("internal error");
            r.status = 500;
            return r;
        }
    }
};
```

**重试**

```cpp
struct RetryOnce {
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        try {
            return next();
        } catch (...) {
            return next();             // 调多次 = 重试
        }
    }
};
```

`next()` 调用几次由切面决定：0 次短路、1 次正常、多次重试。

---

## 5. 挂载

### 5.1 单个 router

```cpp
router r;
r.mount(api);                          // 无切面
r.mount(api, RequestLog{});            // 一个
r.mount(api, RequestLog{}, Auth{});    // 多个：RequestLog 最外层
```

### 5.2 整个应用

```cpp
struct App : application<App> {
    static constexpr auto aspects = std::tuple<RequestLog, Auth>{};
};
```

自动发现的每个 bean 挂载时都会带上这组切面。

### 5.3 非 web / 手工链

```cpp
struct TraceAny {
    template <class Next>
    auto around(const call_info& ci, Next&& next) {
        std::println("enter {}.{}", ci.bean, ci.method);
        auto r = next();
        std::println("leave {}", ci.method);
        return r;
    }
};

aspect_chain<TraceAny> chain{TraceAny{}};
int v = chain.run(call_info{.bean = "Calc", .method = "add"},
                  []() -> int { return 41 + 1; });
```

返回值由最内层调用推导，不限于 `Response`。

单切面也可以直接用 `around` 辅助函数：

```cpp
int v = around(aspect, ci, [] { return 42; });
```

### 5.4 事件监听

事件监听者不是路由，默认不经过切面；`ctx.publish(event, wrap)` 的 wrap 可在
每个监听者调用外接链：

```cpp
aspect_chain<EventTrace> chain{EventTrace{}};
ctx.publish(OrderPlaced{.id = 42},
    [&](std::string_view bean, std::string_view method, auto&& fn) {
        chain.run(call_info{bean, method}, fn);
    });
```

监听者返回 void 时，切面写成 `void around(...)` 即可，`next()` 不返回值。

事件总线的完整用法见 [EventBus.md](EventBus.md)。

### 5.5 方法级选择：`[[=web::UseAspect{"name"}]]`

全局切面对整个 bean / 应用生效。只想让个别路由经过切面时，把切面声明成**命名的 Bean**，
再在 handler 或控制器上按名选择：

```cpp
// 1. 切面是 Bean：可以注入依赖，实例在全部路由间共享
class [[=Bean{}]] HitCounter {
public:
    int hits = 0;
};

class [[=Bean{}]] [[=web::Aspect{"tag"}]] Tagging {
public:
    explicit Tagging(HitCounter& counter) : counter_(counter) {}

    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        ++counter_.hits;
        Response r = next();
        r.header("X-Tag", "1");
        return r;
    }

private:
    HitCounter& counter_;
};

// 2. 在路由 / 控制器上按名选择
class [[=Bean{}]] Api {
public:
    [[=Get{"/plain"}]]
    Response plain();                    // 不经过切面

    [[=Get{"/tagged"}]]
    [[=web::UseAspect{"tag"}]]
    Response tagged();                   // 经过 Tagging
};

// 控制器级：对 AdminApi 的全部路由生效（"deny" 切面省略，写法与 Tagging 同）
class [[=Bean{}]] [[=web::UseAspect{"tag"}]] AdminApi {
public:
    [[=Get{"/admin"}]]
    Response admin();                    // Tagging

    [[=Get{"/admin/all"}]]
    [[=web::UseAspect{"deny"}]]
    Response all();                      // Tagging → Deny
};
```

规则：

- **类级对全部路由生效，方法级追加**，同名只生效一次
- **层次**：全局切面（`App::aspects` / `mount` 的 `aspects...`）→ 类级 → 方法级（标注顺序）→ handler
- **只有带 `[[=web::Aspect{"name"}]]` 的 Bean 可被选择**，且必须同时是 `[[=Bean{}]]`
- **未注册的名字 → 编译错**：
  `cboot: [[=web::UseAspect{"deny"}]] matches no bean with [[=web::Aspect{...}]]`
- **写了 `UseAspect` 却用无注册表的 `mount(obj, ...)` → 编译错**（不会静默失效）
- **切面 Bean 所在 profile 未激活 → 挂载期报错**（不静默跳过）

挂载需要注册表（名称 → 容器里的 Bean，编译期解析类型）。两种常规方式：

```cpp
// 1. 应用引导：自动扫描的 bean 全部带注册表挂载，App::aspects 为全局切面
struct App : application<App> {
    static constexpr auto aspects = std::tuple<RequestLog>{};
};

// 2. 手工挂载到现有 router（单 router 测试、自定义装配）
context<HitCounter, Tagging, Deny, Api, AdminApi> ctx;
router r;
mount_routes(r, ctx, ctx.get<Api>());
mount_routes(r, ctx, ctx.get<AdminApi>(), RequestLog{});   // 追加全局切面
```

`mount_routes(router&, ctx, obj, aspects...)` 是导出入口；`server::mount(obj, registry, aspects...)`
接受任何满足 `web::AspectRegistry` 协议的注册表，应用内部用容器实现它。

### 5.6 生命周期切面

切面包住每个 bean 的 `PostConstruct` / `PreDestroy`，契约是返回 `void`：

```cpp
struct LifecycleTrace {
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        const char* phase = ci.point == join_point::post_construct ? "init" : "destroy";
        std::println("{} {}.{}", phase, ci.bean, ci.method);
        next();
    }
};

// 单个容器（第二参为配置，nullptr = 无）
context<Repo, Svc> ctx(with_lifecycle_aspects, nullptr, LifecycleTrace{});

// 整个应用
struct App : application<App> {
    static constexpr auto lifecycle_aspects = std::tuple<LifecycleTrace>{};
};
```

默认构造的 `context<A, B> ctx;` 不经过切面，行为与之前一致。

规则：

- **切面是容器构造前就存在的值对象**：不能是 Bean、不能注入依赖（此时容器还不存在），
  切面里只能读 `call_info`，不能 `ctx.get<依赖>()`
- **`PostConstruct` 阶段抛异常（切面或方法本体）→ 容器构造失败**，即启动失败
- **`PreDestroy` 阶段抛异常（切面或方法本体）→ 记录到 stderr 后继续销毁其余 bean**：
  析构路径不能抛，否则会 `std::terminate` 并跳过其余 bean
- **不调 `next()` → 跳过该 bean 的生命周期方法**，后果由切面负责
- 启动失败时，已构造 bean 的析构函数照常执行，但不会调用 `PreDestroy`
- 只在启动 / 停机路径执行（经一层 `std::function`），不影响请求期

---

## 6. 状态与并发

切面对象在 `mount` 时按值复制进链：

```cpp
r.mount(api, Counter{});   // Counter 的副本属于这次 mount
```

- 同一个 bean 的所有路由共享同一份链实例
- 不同 bean 各自 mount，得到各自的切面副本
- 用了 `UseAspect` 的 bean 也不例外：全局切面仍是每 bean 一条共享链，不按路由复制
- 多 worker 并发调用同一 bean 时，切面状态需要自己加锁
- 想跨 bean 共享状态，切面成员用 `shared_ptr`：

```cpp
struct Counter {
    std::shared_ptr<std::atomic<int>> hits = std::make_shared<std::atomic<int>>(0);
    template <class Next>
    Response around(const call_info&, Next&& next) {
        hits->fetch_add(1, std::memory_order_relaxed);
        return next();
    }
};
```

切面 Bean（`[[=web::Aspect]]`）不同——实例属于容器：

```cpp
class [[=Bean{}]] [[=web::Aspect{"tag"}]] Tagging {
public:
    int hits = 0;                        // 状态天然跨 bean 共享
    template <class Next>
    Response around(const call_info&, Next&& next) {
        ++hits;                          // 多 worker 下仍需自己加锁
        return next();
    }
};
```

- 链持有切面 Bean 的引用（`aspect_chain<Selected&...>`），不是副本
- 所有选中它的路由——无论属于哪个控制器——共用同一个实例
- 需要的依赖走构造器注入，和其他 Bean 一样

---

## 7. 边界

- **只织入框架调用的位置**：路由 handler、生命周期方法、显式接链的事件监听；
  bean 之间的直接调用不经过切面（C++ 没有隐式代理），需要时用 `aspect_chain` 手工包
- **around 是同步的**：handler 可以返回 `task<Response>`，但 `invoke` 内部用
  `sync_wait` 等它完成，切面看到的是阻塞后的 `Response`；切面链本身还没有
  `co_await` 转发
- **生命周期切面是值对象**：不能是 Bean、不能注入依赖，也不能用 `UseAspect`
  选择切面 Bean（容器构造时 Bean 还不存在）
- **方法级选择按名解析**：名字写错在编译期报错；切面 Bean 所在 profile 未激活
  在挂载期报错——两种情况都不会静默跳过
- **运行期不能增删**：要换切面重新 `mount`
- **异常语义**：handler 抛出的异常会穿过内层切面，外层切面可以捕获；
  没有切面捕获时由服务器转 500

---

## 8. 编译期校验

切面契约可以用概念断言：

```cpp
static_assert(AspectFor<RequestLog, Response, std::function<Response()>>);
static_assert(AspectFor<TraceAny, int, std::function<int()>>);
```

签名不匹配时，`chain->run` 的实例化也会给出编译错误，不会静默跳过。

---

## 9. 性能

- 链在 `mount` 时建立一次，请求期零构造、零分配
- `call_info` 是静态字符串视图，不拼字符串
- 切面调用是模板展开后的直接调用，可内联
- 每请求有一次 `std::function` 类型擦除调用（路由表边界）
- 切面数量增加会线性增加调用层数，但没有虚调用/反射查表

---

## 10. 测试

`test/aop/aop_smoke.cpp` 覆盖：

- 洋葱顺序：`enter:Api.hello` → handler → `leave:hello:200`
- 短路：`Auth` 拒绝 403，外层 `Trace` 仍记录进入/退出
- 异常：`CatchAll` 捕获 handler 异常并转 500
- 非 web：`aspect_chain` 包裹返回 `int` 的可调用
- 概念约束：`AspectFor` 静态断言

`test/aop/method_aspect_smoke.cpp` 覆盖：

- 未标注的路由不经过切面；切面 Bean 的依赖注入生效
- 方法级 / 控制器级选择、重名只生效一次
- 层次：全局切面最外，内层短路时外层仍执行到 after
- 全局切面的值状态在 bean 的全部路由间共享（用了 `UseAspect` 也不例外）
- 切面 Bean 所在 profile 未激活 → 挂载期报错

`test/aop/lifecycle_aspect_smoke.cpp` 覆盖：

- `PostConstruct` / `PreDestroy` 前后拦截，逆拓扑序销毁
- `call_info` 的 bean / method / point
- 短路：不调 `next()` 则生命周期方法不执行
- `PostConstruct` 抛异常 → 容器构造失败；`PreDestroy` 异常隔离
- `App::lifecycle_aspects` → `app_lifecycle_t` 的静态断言与引导链编译

`test/negative/`（预期编译失败）：

- `negative_use_aspect_unregistered.cpp`：`UseAspect` 指向未注册的名字
- `negative_use_aspect_no_registry.cpp`：用了 `UseAspect` 却调用无注册表的 `mount(obj)`

运行：

```bash
cmake --build build-release -j 8
ctest --test-dir build-release -R "aop_smoke|aspect_smoke" --output-on-failure

cmake -B build-neg -DCBOOT_BUILD_NEGATIVE_TESTS=ON
cmake --build build-neg -j 8          # 两个 negative_use_aspect_* 目标应编译失败
```
