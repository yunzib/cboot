# 事件总线

cboot 的事件总线是**进程内、同步、编译期收集监听者**的事件机制。
它不是消息队列：没有持久化、重试、削峰和跨进程投递。

示例默认 `import cboot.app; import std;`（`event_publisher`、`aspect_chain`、
`call_info` 均由 app 模块 re-export）。

相关文件：

| 文件 | 内容 |
|---|---|
| `packages/core/annotations.cppm` | `EventListener` 注解 |
| `packages/core/context.cppm` | 监听者收集、`event_publisher`、`publish`、异常隔离 |
| `test/core/event_smoke.cpp` | 顺序、精确匹配、异常、切面、Profile、publisher 注入 |

---

## 1. 解决什么问题

下单完成后要写审计、发邮件、清缓存。直接调用会让 `OrderService` 依赖所有下游；
每加一个副作用都要改它。

事件总线把"发生了什么"和"谁处理"分开：

- 发布者只认识事件类型
- 监听者各自声明"我关心这个事件"
- 新增监听者不需要改发布者

它适合核心动作之后的旁路逻辑：审计、指标、通知、缓存失效。
不适合需要返回值、强一致、事务回滚或跨进程的场景。

---

## 2. 三步使用

### 2.1 定义事件

普通结构体即可：

```cpp
struct OrderPlaced {
    int id{};
};
```

### 2.2 写监听方法

方法加 `[[=EventListener{}]]`，参数类型就是订阅的事件类型：

```cpp
class [[=Bean{}]] Audit {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced& e) {
        std::println("[audit] order {}", e.id);
    }
};

class [[=Bean{}]] Mail {
public:
    [[=EventListener{}]]
    void on(OrderPlaced e) {
        std::println("[mail] order {}", e.id);
    }
};
```

约束：

- 方法必须 public、非静态
- 恰好一个参数；按值或 `const&`
- 参数去掉 cv/引用后必须与事件类型精确相等

### 2.3 在业务 bean 中发布

注入内建依赖 `event_publisher&`：

```cpp
class [[=Bean{}]] OrderService {
public:
    explicit OrderService(event_publisher& bus) : bus_(bus) {}

    void place(int id) {
        // 核心动作
        (void)bus_.publish(OrderPlaced{.id = id});
    }

private:
    event_publisher& bus_;
};
```

控制器调用它：

```cpp
class [[=Bean{}]] OrderApi {
public:
    explicit OrderApi(OrderService& svc) : svc_(svc) {}

    [[=Post{"/orders/{id}"}]]
    Response create(int id) {
        svc_.place(id);
        return json_response(id);
    }

private:
    OrderService& svc_;
};

class App : public application<App> {
public:
    static constexpr unsigned short default_port = 8080;
};

int main() { return App{}.run(); }
```

`event_publisher` 是容器提供的**内建依赖**：

- 不需要 `[[=Bean{}]]`
- 不参与自动发现
- 由容器在启动时绑定，业务 bean 直接注入引用

测试或手工编排也可以直接调用容器：

```cpp
context<Audit, Mail> ctx;
event_result r = ctx.publish(OrderPlaced{.id = 42});
// r.delivered == 2, r.failed == 0
```

---

## 3. 派发规则

| 项 | 规则 |
|---|---|
| 匹配 | 参数去掉 cv/引用后与事件类型精确相等；不走基类/派生匹配 |
| 顺序 | bean 清单顺序 → 方法声明顺序；稳定，不是优先级 |
| Profile | 未激活 bean 的监听者跳过 |
| 返回 | `event_result{delivered, failed}`；无匹配监听者时为 0 |
| 线程 | 同步调用；并发发布由调用方保证监听者线程安全 |

监听者方法的返回值被忽略；事件派发没有返回值语义。

---

## 4. 异常处理

### 4.1 作用

监听者是普通代码：邮件服务可能超时，审计可能写盘失败。默认策略让一次失败
**只影响它自己**：

- 发布者不会收到异常（`publish` 不重新抛）
- 后面的监听者照常执行
- 留下可观测记录（默认 stderr，可替换）
- 返回值里的 `failed` 告诉发布者这次有多少监听者失败

### 4.2 默认行为：不写任何代码

`publish` 内部对每个监听者单独 `try/catch`：

1. 捕获当前监听者的异常
2. 向 stderr 写一行：`[cboot] event listener Audit.on failed: ...`
3. `event_result.failed` 加一
4. 继续调用后面的监听者
5. 不把异常重新抛给发布者

例子：`Audit` 抛异常，`Mail` 正常：

```cpp
class [[=Bean{}]] Audit {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced&) {
        throw std::runtime_error("audit down");
    }
};

class [[=Bean{}]] Mail {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced& e) {
        std::println("mail {}", e.id);
    }
};

context<Audit, Mail> ctx;
event_result r = ctx.publish(OrderPlaced{.id = 42});
// r.delivered == 1
// r.failed == 1
// Mail.on 仍然执行
// publish 正常返回，不抛异常
```

默认日志：

```
[cboot] event listener Audit.on failed: audit down
```

### 4.3 自定义上报：什么时候需要

默认 stderr 适合开发阶段。生产通常要接应用日志、指标或告警；测试里可能想
断言“哪个监听者失败了”。

两种设置入口：

**测试 / 手工容器**

```cpp
context<Audit, Mail> ctx;
ctx.set_event_error_handler(
    [](std::string_view bean, std::string_view method, std::string_view what) {
        // 写日志、计数、告警……
        std::println("listener {}.{} failed: {}", bean, method, what);
    });
```

**业务 bean（自动发现）**

```cpp
class [[=Bean{}]] ErrorReporter {
public:
    explicit ErrorReporter(event_publisher& bus) {
        bus.set_error_handler([](std::string_view bean,
                                 std::string_view method,
                                 std::string_view what) {
            // 同上
        });
    }
};
```

容器构造 `ErrorReporter` 后，之后所有 `publish` 都走这个处理器。

回调参数：

| 参数 | 含义 | 本例 |
|---|---|---|
| `bean` | 监听者所在 bean 类型名 | `Audit` |
| `method` | 监听方法名 | `on` |
| `what` | `std::exception::what()`；非 std 异常为 `"unknown exception"` | `audit down` |

注意：

- 回调在 `publish` 内部同步执行，保持轻量
- 回调自身抛异常会被捕获并退回 stderr，不影响后续监听者
- 它只替换“怎么记录”，不改变“继续派发”的策略
- 发布方不关心失败时可以忽略 `event_result`；这时处理器/默认日志就是唯一信号
- 监听者自己 `catch` 掉的异常、或切面吞掉的异常，不会计入 `failed`
- 需要“任一监听者失败就回滚/让请求失败”的语义，不要用事件总线；
  直接调用并让异常上抛

---

## 5. 与 AOP 组合

### 5.1 作用

每个监听者都手写耗时日志、trace id 很重复。切面把这段横切逻辑集中到一处。

一次 `publish` 会为**每个匹配的监听者**各包一次，不是包整个 publish 一次：

```
EventTrace: enter Audit.on
  Audit.on
EventTrace: leave Audit.on
EventTrace: enter Mail.on
  Mail.on
EventTrace: leave Mail.on
```

适合：统一耗时、日志、追踪、测试观察顺序、按条件跳过监听者。

### 5.2 怎么用

**1）定义切面**：监听者返回 void，切面也返回 void。

```cpp
class EventTrace {
public:
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        std::println("enter {}.{}", ci.bean, ci.method);
        next();
        std::println("leave {}.{}", ci.bean, ci.method);
    }
};
```

**2）创建 chain，并在 `ctx.publish` 的 wrap 中调用**：

```cpp
context<Audit, Mail> ctx;                 // 2.2 的正常版本
aspect_chain<EventTrace> chain{EventTrace{}};

ctx.publish(OrderPlaced{.id = 42},
    [&](std::string_view bean, std::string_view method, auto&& fn) {
        chain.run(call_info{bean, method}, fn);
    });
```

输出：

```
enter Audit.on
[audit] order 42
leave Audit.on
enter Mail.on
[mail] order 42
leave Mail.on
```

### 5.3 规则

- wrap 每个匹配监听者调用一次，顺序与派发顺序一致
- wrap 不调用 `fn()`：跳过该监听者
- wrap 调用多次 `fn()`：重试（注意副作用）
- wrap / 切面抛异常：按第 4 节处理，`failed++`，继续下一个
- 注入式 `event_publisher::publish` 是直接派发，没有 wrap 参数；
  需要切面时用 `ctx.publish(event, wrap)`
- `call_info.bean / .method` 由框架传入；切面细节见 [AOP.md](AOP.md)

---

## 6. 边界

- 同步、进程内；没有持久化、重试、削峰、跨进程投递
- 没有返回值与事务语义；监听者异常默认不影响发布
- 只按精确事件类型匹配；派生事件不会触发基类监听者
- 顺序由声明决定，没有优先级和运行期订阅/退订
- `event_publisher` 只用于容器构造的 bean；手工场景用 `ctx.publish`
