# AOP 切面

cboot 的 AOP 是**编译期展开的静态调用链**：没有运行时代理、没有虚调用、没有每次请求的链分配。
切入点只有路由 handler——也就是框架真正控制调用的地方。

相关文件：

| 文件 | 内容 |
|---|---|
| `modules/aop/aop.cppm` | `call_info` / `AspectFor` / `aspect_chain` / `around` |
| `modules/web/routing.cppm` | `router::mount`、handler 包装、`call_info` 生成 |
| `test/aop_smoke.cpp` | 洋葱顺序、短路、异常、非 web、概念约束 |
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

---

## 6. 状态与并发

切面对象在 `mount` 时按值复制进链：

```cpp
r.mount(api, Counter{});   // Counter 的副本属于这次 mount
```

- 同一个 bean 的所有路由共享同一份链实例
- 不同 bean 各自 mount，得到各自的切面副本
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

---

## 7. 边界

- **只织入路由 handler**：bean 之间的直接调用不经过切面（C++ 没有隐式代理）；
  需要时显式用 `aspect_chain` 包裹
- **around 是同步的**：handler 可以返回 `task<Response>`，但 `invoke` 内部用
  `sync_wait` 等它完成，切面看到的是阻塞后的 `Response`；切面链本身还没有
  `co_await` 转发
- **生命周期不织入**：`PostConstruct` / `PreDestroy` 不经过切面
- **没有方法级选择**：切面是 mount 级别的整组，不能只挂某个路由
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

`test/aop_smoke.cpp` 覆盖：

- 洋葱顺序：`enter:Api.hello` → handler → `leave:hello:200`
- 短路：`Auth` 拒绝 403，外层 `Trace` 仍记录进入/退出
- 异常：`CatchAll` 捕获 handler 异常并转 500
- 非 web：`aspect_chain` 包裹返回 `int` 的可调用
- 概念约束：`AspectFor` 静态断言

运行：

```bash
cmake --build build-release -j 8
ctest --test-dir build-release -R aop_smoke --output-on-failure
```
