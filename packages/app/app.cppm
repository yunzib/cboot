export module cboot.app;

import std;

// ── 对外的模块面：re-export（消费方仅需 `import cboot.app;`）──
// 注：CMake 的导入模块集不向消费方 TU 传递——re-export 让 app 成为
// 单一入口模块（import 一个即得全部框架词汇）。
export import cboot.aop;                // call_info / AspectFor / aspect_chain
export import cboot.config;             // config::properties / bind
export import cboot.core.annotations;   // Bean / Inject / Config / 生命周期
export import cboot.core.metrics;       // metrics::global()（指标注册表）
export import cboot.core.context;       // context / context_for
export import cboot.json;               // json::value / parse / from
export import cboot.log;                // logging::get / logger / sinks
export import cboot.log.component;      // log_component / LogProps
export import cboot.meta.reflect;       // for_each_member / type_list / type_of_t
export import cboot.validation;         // NotBlank / Range / validate
export import cboot.web.http;           // Get/Post/Put/Delete/Patch / Request / Response
export import cboot.web.middleware;     // Middleware / UseMiddleware / request_context
export import cboot.web.access;         // AccessLog / request_id（请求关联层）
export import cboot.web.error;          // error_info / ErrorMapper / ErrorHandler
export import cboot.web.ratelimit;      // RateLimiter / rate_limit_options
export import cboot.web.routing;        // router
export import cboot.web.server;         // server

/**
 * @brief 应用层 — 引导编排（自动发现 / 装配 / 启动）
 *
 * 分层：app → web.server → routing/http → core/json/aop → meta
 *   - web.server 只负责 HTTP 传输（accept/解析/写响应）
 *   - 本模块负责"应用"语义：扫描 bean、构造容器、挂载路由、生命周期、启动
 *
 * 自动发现（Spring 组件扫描的 C++ 对应物，反射实现）：
 *   - 扫描**全局命名空间**：凡当前 TU 可见、显式标注 [[=Bean{}]] 或
 *     [[=Config{...}]] 的完整类类型，自动纳入清单
 *   - 单一规则：控制器也是 bean，必须标 [[=Bean{}]]（带路由方法却漏标 →
 *     编译错并指出类型名，杜绝隐式规则与静默 404）
 *   - 可见性边界（模块语义）：本 TU 直接声明，或 import 的模块 export 导出
 *   - 非注解类型（DTO/工具类）自动排除；容量上限 128
 *
 * 实例化纪律（GCC16 模块实测）：
 *   扫描标签（Tag/App）必须**依赖调用方类型**，否则 auto_beans_t<...> 是
 *   非依赖特化，会在模块编译期实例化——那时只见模块自身声明（扫描 = 0）。
 */

// 全局命名空间反射锚点：必须位于模块**全局作用域**——
// 放进 namespace detail 会让 parent_of 返回 detail 命名空间，扫描落空
struct scan_anchor {};

/**
 * @brief 内建运维端点（应用启动时自动挂载）
 *
 *   GET /health  → 200 UP / 503 DOWN，JSON：status/beans/startup_ms/reason
 *   GET /beans   → 仅 dev profile：bean 清单、激活状态、依赖
 *   GET /livez   → 200（进程可服务即通过；不依赖任何外部资源）
 *   GET /readyz  → 200 READY / 503 NOT_READY（依赖健康：DB 等 DOWN 时摘流量）
 *   GET /startupz→ 200（容器构造完成；供 k8s startupProbe 使用）
 *
 * 探针语义（k8s 三探针）：
 *   - liveness 失败 → 重启进程；因此 /livez 永不看依赖状态
 *   - readiness 失败 → 从 Service 摘除，不重启；看 health_registry
 *     （驱动建连失败会 set_unhealthy，恢复后 set_healthy）
 *   - startup 失败 → 重启并放慢重试；本框架端点挂载发生在容器构造
 *     之后，能访问即已启动完成，故 /startupz 恒 200
 *
 * 用户不需要实例化；`boot_runner` 会挂到 router 上。
 */
export template <class Ctx>
class health_endpoint {
public:
    explicit health_endpoint(Ctx& ctx) : ctx_(ctx) {}

    [[=Get{"/health"}]]
    Response health() {
        const health_snapshot h = ctx_.health();
        std::string body;
        body += "{\"status\":\"";
        body += h.healthy ? "UP" : "DOWN";
        body += "\",\"beans\":";
        body += std::to_string(ctx_.active_bean_count());
        body += ",\"startup_ms\":";
        body += std::to_string(ctx_.startup_ms());
        if (!h.healthy) {
            body += ",\"reason\":";
            json::write_escaped(body, h.reason);
        }
        body += "}";
        Response r;
        r.status = h.healthy ? 200 : 503;
        r.body = std::move(body);
        return r;
    }

    /** @brief liveness：进程能处理请求即 UP（绝不再看依赖，避免误重启） */
    [[=Get{"/livez"}]]
    Response livez() {
        Response r;
        r.body = "{\"status\":\"UP\"}";
        return r;
    }

    /** @brief readiness：依赖健康才 READY；否则 503 + reason（摘流量不重启） */
    [[=Get{"/readyz"}]]
    Response readyz() {
        const health_snapshot h = ctx_.health();
        std::string body;
        body += "{\"status\":\"";
        body += h.healthy ? "READY" : "NOT_READY";
        body += "\"";
        if (!h.healthy) {
            body += ",\"reason\":";
            json::write_escaped(body, h.reason);
        }
        body += "}";
        Response r;
        r.status = h.healthy ? 200 : 503;
        r.body = std::move(body);
        return r;
    }

    /** @brief startup：能响应即表示容器构造完成（恒 200） */
    [[=Get{"/startupz"}]]
    Response startupz() {
        Response r;
        r.body = "{\"status\":\"STARTED\"}";
        return r;
    }

    [[=Get{"/beans"}]]
    Response beans() {
        if (!ctx_.has_profile("dev")) {
            Response r = Response::text("not found");
            r.status = 404;
            return r;
        }
        std::string body = "[";
        bool first = true;
        for (const auto& b : ctx_.beans_info()) {
            if (!first) body += ',';
            first = false;
            body += "{\"type\":";
            json::write_escaped(body, b.type);
            body += ",\"name\":";
            json::write_escaped(body, b.name);
            body += ",\"profile\":";
            json::write_escaped(body, b.profile);
            body += ",\"active\":";
            body += b.active ? "true" : "false";
            body += ",\"deps\":[";
            bool dep_first = true;
            for (auto d : b.deps) {
                if (!dep_first) body += ',';
                dep_first = false;
                json::write_escaped(body, d);
            }
            body += "]}";
        }
        body += "]";
        Response r;
        r.body = std::move(body);
        return r;
    }

private:
    Ctx& ctx_;
};

/**
 * @brief 内建指标端点 — GET /metrics（Prometheus 文本格式 0.0.4）
 *
 * 渲染 metrics::global() 全部系列（web 记录请求指标，ORM 等后续可
 * 自行注册池指标）+ 应用级 gauge（beans / startup_ms / uptime）。
 * k8s/Prometheus 抓取即开即用；无敏感数据（路由模板 + 状态大类）。
 * uptime 锚点是本进程内首次调用，非进程启动精确值（文档已注明）。
 */
export template <class Ctx>
class metrics_endpoint {
public:
    explicit metrics_endpoint(Ctx& ctx) : ctx_(ctx) {}

    [[=Get{"/metrics"}]]
    Response metrics() {
        std::string body = metrics::global().render_prometheus();
        // 应用级 gauge：直接拼文本（值随时间/容器状态变化，不入注册表）
        static const auto started = std::chrono::steady_clock::now();
        const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        body += "# TYPE cboot_uptime_ms gauge\n";
        body += "cboot_uptime_ms " + std::to_string(uptime_ms.count()) + "\n";
        body += "# TYPE cboot_beans gauge\n";
        body += "cboot_beans " + std::to_string(ctx_.active_bean_count()) + "\n";
        body += "# TYPE cboot_startup_ms gauge\n";
        body += "cboot_startup_ms " + std::to_string(ctx_.startup_ms()) + "\n";

        Response r;
        r.content_type = "text/plain; version=0.0.4; charset=utf-8";
        r.body = std::move(body);
        return r;
    }

private:
    Ctx& ctx_;
};

namespace detail {

// 优雅停机：控制台中断/关闭事件 → server.stop()（实现于 src/app/app.cpp）
void install_stop_handler(server& s);
void remove_stop_handler();

constexpr std::size_t scan_capacity = 128;

/** @brief 全局命名空间反射 */
consteval std::meta::info global_ns() { return std::meta::parent_of(^^scan_anchor); }

/** @brief T 是否带路由方法注解（控制器判据，无需 [[=Bean{}]]） */
consteval bool has_route_methods(std::meta::info T) {
    for (auto m : std::meta::members_of(T, std::meta::access_context::unchecked()))
        if (std::meta::is_function(m)
            && (std::meta::annotations_of_with_type(m, ^^Get).size() > 0
                || std::meta::annotations_of_with_type(m, ^^Post).size() > 0))
            return true;
    return false;
}

/** @brief 扫描候选：完整类类型，且显式标注 [[=Bean{}]] 或 [[=Config{...}]] */
consteval bool is_scan_candidate(std::meta::info m) {
    if (!std::meta::is_type(m) || std::meta::is_template(m)) return false;
    if (!std::meta::is_complete_type(m)) return false;
    if (!std::meta::is_class_type(m)) return false;   // 免 type_of_t splice
    return std::meta::annotations_of_with_type(m, ^^Bean).size() > 0
        || std::meta::annotations_of_with_type(m, ^^Config).size() > 0;
}

/**
 * @brief 控制器显式性校验：带路由方法却未标注 [[=Bean{}]] → 编译错
 *
 * 单一规则：bean 一律显式标注。避免"有路由方法就算 bean"的隐式第二路径
 * （读代码无法确认 bean 身份，也掩盖漏标导致的静默 404）。诊断消息带
 * 类型名（define_static_string：编译期生成静态字符串）。
 */
consteval void validate_route_owner(std::meta::info m) {
    if (std::meta::annotations_of_with_type(m, ^^Bean).size() > 0) return;
    if (!has_route_methods(m)) return;
    throw std::define_static_string(
        std::string("cboot: '") + std::string(std::meta::identifier_of(m))
        + "' has route methods but lacks [[=Bean{}]] annotation");
}

/**
 * @brief 扫描结果：固定容量数组 + 计数（声明身份去重）
 *
 * 必须模板化：consteval 求值在**实例化点**（消费方 TU）进行，才能看到
 * 消费方的全局声明；非模板 consteval 在模块定义期求值，只见模块自身。
 */
template <class Tag>
consteval auto scan_visible_beans() {
    std::array<std::meta::info, scan_capacity> out{};
    std::size_t n = 0;
    for (auto m : std::meta::members_of(global_ns(), std::meta::access_context::unchecked())) {
        if (std::meta::is_type(m) && !std::meta::is_template(m)
            && std::meta::is_complete_type(m) && std::meta::is_class_type(m))
            validate_route_owner(m);                   // 显式性校验（控制器必须标 Bean）
        if (!is_scan_candidate(m)) continue;
        bool dup = false;
        for (std::size_t i = 0; i < n; ++i)
            if (out[i] == m) { dup = true; break; }
        if (dup) continue;
        if (n >= scan_capacity)
            throw std::define_static_string(
                std::string("cboot: auto-discovery exceeded capacity ")
                + std::to_string(scan_capacity)
                + " beans — split modules or use explicit run<Beans...>()");
        out[n++] = m;
    }
    return std::pair{out, n};
}

/** @brief 清单执行器：容器构造 → 组件 hooks → 挂载路由（可带切面）→ 服务 */
template <class Beans, class Hooks = type_list<>>
struct boot_runner;

/** @brief 注册带 [[=web::Middleware]] 的 bean（支持全局 / 路由级） */
template <class Ctx, class B, std::size_t I>
void register_middleware(server& s, Ctx& ctx) {
    constexpr auto mw = annotation_of<web::Middleware>(^^B);
    if constexpr (mw.has_value()) {
        static_assert(web::MiddlewareHandler<B>,
                      "cboot.app: [[=Middleware]] bean must provide "
                      "handle(request_context&, const next_fn&)");
        if (ctx.template is_active<I>()) {
            s.use(std::string(mw->name.view()), mw->order,
                  [&ctx](web::request_context& rc, const web::next_fn& next) {
                      return ctx.template get<B>().handle(rc, next);
                  });
        }
    }
}

template <class Ctx, class... Bs>
void register_middlewares(server& s, Ctx& ctx) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (register_middleware<Ctx, typename Ctx::template bean_type<I>, I>(s, ctx),
         ...);
    }(std::make_index_sequence<sizeof...(Bs)>{});
}

/** @brief 注册带 [[=web::ErrorMapper]] 的 bean（按 order 升序） */
template <class Ctx, class B, std::size_t I>
void register_error_mapper(web::error_registry& errors, Ctx& ctx) {
    constexpr auto mapper = annotation_of<web::ErrorMapper>(^^B);
    if constexpr (mapper.has_value()) {
        static_assert(web::ErrorMapperHandler<B>,
                      "cboot.app: [[=ErrorMapper]] bean must provide "
                      "handle(const error_info&, const request_context&) "
                      "-> std::optional<Response>");
        if (ctx.template is_active<I>()) {
            errors.add(mapper->order,
                [&ctx](const web::error_info& info,
                       const web::request_context& rc) {
                    return ctx.template get<B>().handle(info, rc);
                });
        }
    }
}

template <class Ctx, class... Bs>
void register_error_mappers(web::error_registry& errors, Ctx& ctx) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (register_error_mapper<Ctx, typename Ctx::template bean_type<I>, I>(
             errors, ctx),
         ...);
    }(std::make_index_sequence<sizeof...(Bs)>{});
}

template <class... Ts, class... Hs>
struct boot_runner<type_list<Ts...>, type_list<Hs...>> {
    template <class... As>
    static int boot(unsigned short port, std::size_t workers, server_io io,
                    std::size_t max_conns, const config::properties* cfg,
                    As... aspects) {
        context<Ts...> ctx(cfg);                  // 先构造：先于 server 启动
        // 组件 hooks 在全部 bean 构造完后安装；先于 ctx 析构停止。
        hook_scope<context<Ts...>, Hs...> hooks(ctx);

        auto app_log = logging::get("cboot.app");
        app_log.info("starting on port {} (workers={}, beans={})",
                     port, workers, sizeof...(Ts));
        server_config sc;
        sc.port = port;
        sc.workers = workers;
        sc.io = io;
        sc.max_connections = max_conns;
        if (cfg) {                                // 超时配置（内联解析，见导出纪律）
            if (auto v = cfg->get_expanded("server.handler_timeout_ms")) {
                try { sc.handler_timeout_ms = std::stoul(std::string(*v)); } catch (...) {}
            }
            if (auto v = cfg->get_expanded("server.send_timeout_ms")) {
                try { sc.send_timeout_ms = std::stoul(std::string(*v)); } catch (...) {}
            }
        }
        // 全局错误层：registry 与 handler 先于 server 构造，保证生命周期覆盖 run。
        web::error_registry errors;
        register_error_mappers<context<Ts...>, Ts...>(errors, ctx);
        web::ErrorHandler error_handler(errors);

        server s(sc);
        install_stop_handler(s);                  // Ctrl+C / 关闭 → 优雅停机
        // 访问日志层（最外）：request_id + 单行 JSON access log。
        // 顺序 -950：ErrorHandler(-900) 外层——错误转换后的最终状态码与
        // 总耗时都能记录；级别/关闭经 log.logger_levels 前缀控制。
        web::AccessLog access_log;
        s.use("", web::access_middleware_order,
              [&access_log](web::request_context& rc,
                            const web::next_fn& next) {
                  return access_log.handle(rc, next);
              });
        s.use("", web::error_middleware_order,
              [&error_handler](web::request_context& rc,
                               const web::next_fn& next) {
                  return error_handler.handle(rc, next);
              });
        register_middlewares<context<Ts...>, Ts...>(s, ctx);
        // 内建运维端点：/health /beans（health）/ /metrics（指标）。
        // 按清单下标挂载：同基类的多个 bean 用类型 get 会歧义；
        health_endpoint<context<Ts...>> ops(ctx);
        metrics_endpoint<context<Ts...>> ops_metrics(ctx);
        s.mount(ops);
        s.mount(ops_metrics);
        // 未激活 profile 的 bean 不挂载。
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([&] {
                if (ctx.template is_active<Is>())
                    s.mount(ctx.template get_at<Is>(), aspects...);
            }(), ...);
        }(std::make_index_sequence<sizeof...(Ts)>{});
        int rc = s.run();
        app_log.info("stopped (code={})", rc);
        remove_stop_handler();
        return rc;                                // ctx 析构 → PreDestroy（逆序）
    }
};

} // namespace detail

// ==================== 自动发现清单 ====================

export template <class Tag = void>
consteval std::size_t auto_bean_count() {
    return detail::scan_visible_beans<Tag>().second;
}

export template <std::size_t I, class Tag = void>
consteval std::meta::info auto_bean_at() {
    return detail::scan_visible_beans<Tag>().first[I];
}

export template <class Tag, std::size_t... I>
consteval auto auto_list_type(std::index_sequence<I...>) {
    return ^^type_list<type_of_t<auto_bean_at<I, Tag>()>...>;
}

/**
 * @brief 自动发现清单类型（当前 TU 可见的 [[=Bean{}]] / 控制器）
 *
 *   auto_beans_t<> 展开即 type_list<...>；context_for<auto_beans_t<>> 得容器
 */
export template <class Tag = void>
using auto_beans_t = [: auto_list_type<Tag>(
    std::make_index_sequence<auto_bean_count<Tag>()>{}) :];

// ==================== 契约组件 ====================

namespace detail {

// 组件描述符 shape：beans 必有；dependencies/hooks 可缺省，存在时必须是 type_list
template <class C, class = void>
struct beans_list_ok : std::false_type {};
template <class C>
struct beans_list_ok<C, std::void_t<typename C::beans>>
    : std::bool_constant<is_type_list_v<typename C::beans>> {};

template <class C, class = void>
struct dependencies_list_ok : std::true_type {};
template <class C>
struct dependencies_list_ok<C, std::void_t<typename C::dependencies>>
    : std::bool_constant<is_type_list_v<typename C::dependencies>> {};

template <class C, class = void>
struct hooks_list_ok : std::true_type {};
template <class C>
struct hooks_list_ok<C, std::void_t<typename C::hooks>>
    : std::bool_constant<is_type_list_v<typename C::hooks>> {};

} // namespace detail

/**
 * @brief 组件描述符 shape：普通结构体带 beans，可选 dependencies / hooks
 *
 * 不是运行期接口；只约束描述符字段类型。真正的装配语义由
 * component_closure_t / app_beans_t / app_hooks_t 在编译期展开。
 */
export template <class C>
concept Component = detail::beans_list_ok<C>::value
    && detail::dependencies_list_ok<C>::value
    && detail::hooks_list_ok<C>::value;

namespace detail {

constexpr std::size_t component_capacity = 64;

consteval bool is_type_list_info(std::meta::info t) {
    return std::meta::has_template_arguments(t)
        && std::meta::template_of(t) == ^^type_list;
}

consteval std::optional<std::meta::info> alias_target(
    std::meta::info c, std::string_view name) {
    for (auto m : std::meta::members_of(c, std::meta::access_context::unchecked()))
        if (std::meta::is_type_alias(m) && std::meta::identifier_of(m) == name)
            return std::meta::dealias(m);
    return std::nullopt;
}

consteval std::string_view component_name(std::meta::info c) {
    return std::meta::has_identifier(c) ? std::meta::identifier_of(c) : "?";
}

consteval bool is_component_shape(std::meta::info c) {
    if (!std::meta::is_type(c) || std::meta::is_template(c)
        || !std::meta::is_complete_type(c) || !std::meta::is_class_type(c))
        return false;
    auto beans = alias_target(c, "beans");
    if (!beans || !is_type_list_info(*beans)) return false;
    auto deps = alias_target(c, "dependencies");
    if (deps && !is_type_list_info(*deps)) return false;
    auto hooks = alias_target(c, "hooks");
    if (hooks && !is_type_list_info(*hooks)) return false;
    return true;
}

/** @brief 直接依赖的组件反射；缺失 dependencies 退化为空 */
consteval auto component_deps(std::meta::info c) {
    std::array<std::meta::info, component_capacity> out{};
    std::size_t n = 0;
    auto deps = alias_target(c, "dependencies");
    if (!deps) return std::pair{out, n};
    for (auto d : std::meta::template_arguments_of(*deps)) {
        if (!is_component_shape(d))
            throw std::define_static_string(
                "cboot: component '" + std::string(component_name(c))
                + "' depends on '" + std::string(component_name(d))
                + "' which is not a Component");
        if (n >= component_capacity)
            throw std::define_static_string(
                "cboot: component closure exceeded capacity "
                + std::to_string(component_capacity));
        out[n++] = d;
    }
    return std::pair{out, n};
}

template <class App>
struct app_component_roots {
    using type = type_list<>;
};
template <class App>
    requires requires { typename App::components; }
struct app_component_roots<App> {
    using type = typename App::components;
};

template <class L>
struct type_list_infos;
template <class... Ts>
struct type_list_infos<type_list<Ts...>> {
    static constexpr std::array<std::meta::info, sizeof...(Ts)> value{^^Ts...};
};

/**
 * @brief 组件闭包：DFS 后序，依赖在前、自身在后；菱形去重、环诊断
 *
 * 在应用 TU 实例化（依赖 App）；只处理描述符类型，不构造 bean。
 */
template <class App>
consteval auto component_closure_impl() {
    constexpr auto roots =
        type_list_infos<typename app_component_roots<App>::type>::value;

    std::array<std::meta::info, component_capacity> out{};
    std::array<std::meta::info, component_capacity> stack{};
    std::array<std::meta::info, component_capacity> seen{};
    std::array<int, component_capacity> state{};
    std::size_t n = 0;       // out 数量
    std::size_t depth = 0;   // DFS 栈深度
    std::size_t seen_n = 0;

    auto index_of = [&](std::meta::info c) -> std::size_t {
        for (std::size_t i = 0; i < seen_n; ++i)
            if (seen[i] == c) return i;
        if (seen_n >= component_capacity)
            throw std::define_static_string(
                "cboot: component closure exceeded capacity "
                + std::to_string(component_capacity));
        seen[seen_n] = c;
        state[seen_n] = 0;
        return seen_n++;
    };

    auto visit = [&](auto&& self, std::meta::info c) -> void {
        if (!is_component_shape(c))
            throw std::define_static_string(
                "cboot: '" + std::string(component_name(c))
                + "' is not a Component (missing beans/dependencies type list)");

        const std::size_t idx = index_of(c);
        if (state[idx] == 1) {                    // 回边 → 组件环
            std::string msg = "cboot: component dependency cycle: ";
            std::size_t start = 0;
            while (start < depth && stack[start] != c) ++start;
            for (std::size_t i = start; i < depth; ++i) {
                msg += component_name(stack[i]);
                msg += " -> ";
            }
            msg += component_name(c);
            throw std::define_static_string(msg);
        }
        if (state[idx] == 2) return;              // 菱形依赖已访问

        state[idx] = 1;
        if (depth >= component_capacity)
            throw std::define_static_string(
                "cboot: component closure exceeded capacity "
                + std::to_string(component_capacity));
        stack[depth++] = c;

        auto deps = component_deps(c);
        for (std::size_t i = 0; i < deps.second; ++i)
            self(self, deps.first[i]);

        --depth;
        state[idx] = 2;
        if (n >= component_capacity)
            throw std::define_static_string(
                "cboot: component closure exceeded capacity "
                + std::to_string(component_capacity));
        out[n++] = c;                             // 后序：依赖先进入结果
    };

    for (auto r : roots) visit(visit, r);
    return std::pair{out, n};
}

template <class App>
consteval std::size_t component_count() {
    return component_closure_impl<App>().second;
}

template <std::size_t I, class App>
consteval std::meta::info component_at() {
    return component_closure_impl<App>().first[I];
}

template <class App, std::size_t... I>
consteval auto component_list_type(std::index_sequence<I...>) {
    return ^^type_list<type_of_t<component_at<I, App>()>...>;
}

template <class L>
struct component_beans_impl;
template <class... Cs>
struct component_beans_impl<type_list<Cs...>> {
    using type = concat_t<typename Cs::beans...>;
};

template <class C>
struct component_hooks_of {
    using type = type_list<>;
};
template <class C>
    requires requires { typename C::hooks; }
struct component_hooks_of<C> {
    using type = typename C::hooks;
};

template <class L>
struct component_hooks_impl;
template <class... Cs>
struct component_hooks_impl<type_list<Cs...>> {
    using type = concat_t<typename component_hooks_of<Cs>::type...>;
};

template <class L>
struct components_valid;
template <class... Cs>
struct components_valid<type_list<Cs...>>
    : std::bool_constant<(Component<Cs> && ...)> {};

/**
 * @brief 组件 bean 契约校验（编译期）
 *
 * - 完整、非模板类类型
 * - 不能是全局命名空间直接成员（否则 import 即被自动扫描）
 * - 必须有 [[=Bean{}]] 或 [[=Config{...}]]
 * - 控制器类路由方法仍要求 [[=Bean{}]]
 * - 同一组件 beans 列表不允许重复类型
 */
consteval void validate_component_beans(std::meta::info c) {
    auto beans = alias_target(c, "beans");
    if (!beans) return;

    std::array<std::meta::info, scan_capacity> seen{};
    std::size_t n = 0;

    for (auto b : std::meta::template_arguments_of(*beans)) {
        if (!std::meta::is_type(b) || std::meta::is_template(b)
            || !std::meta::is_complete_type(b)
            || !std::meta::is_class_type(b))
            throw std::define_static_string(
                "cboot: component '" + std::string(component_name(c))
                + "' bean '" + std::string(component_name(b))
                + "' must be a complete class type");

        if (std::meta::parent_of(b) == global_ns())
            throw std::define_static_string(
                "cboot: component bean '" + std::string(component_name(b))
                + "' must not be a direct member of the global namespace");

        const bool annotated =
            std::meta::annotations_of_with_type(b, ^^Bean).size() > 0
            || std::meta::annotations_of_with_type(b, ^^Config).size() > 0;
        if (!annotated)
            throw std::define_static_string(
                "cboot: component '" + std::string(component_name(c))
                + "' bean '" + std::string(component_name(b))
                + "' lacks [[=Bean{}]] or [[=Config{...}]]");

        validate_route_owner(b);

        for (std::size_t i = 0; i < n; ++i)
            if (seen[i] == b)
                throw std::define_static_string(
                    "cboot: component '" + std::string(component_name(c))
                    + "' lists bean '" + std::string(component_name(b))
                    + "' more than once");

        if (n >= scan_capacity)
            throw std::define_static_string(
                "cboot: component bean list exceeded capacity "
                + std::to_string(scan_capacity));
        seen[n++] = b;
    }
}

template <class L>
struct validate_components_impl;
template <class... Cs>
struct validate_components_impl<type_list<Cs...>> {
    static consteval void run() {
        (validate_component_beans(^^Cs), ...);
    }
};

/** @brief type_list<Ts...> → 反射 info（保持 type_list 形状） */
template <class L>
struct list_info;
template <class... Ts>
struct list_info<type_list<Ts...>> {
    static consteval std::meta::info make() { return ^^type_list<Ts...>; }
};

} // namespace detail

/** @brief 应用声明的根组件清单；未声明退化为空 */
export template <class App>
using app_components_t = typename detail::app_component_roots<App>::type;

/** @brief 组件闭包：依赖在前、自身在后 */
export template <class App>
using component_closure_t = [: detail::component_list_type<App>(
    std::make_index_sequence<detail::component_count<App>()>{}) :];

/** @brief 闭包内所有组件贡献的 bean（拼接） */
export template <class L>
using component_beans_t = typename detail::component_beans_impl<L>::type;

/** @brief 闭包内所有组件贡献的 hooks（拼接） */
export template <class L>
using component_hooks_t = typename detail::component_hooks_impl<L>::type;

namespace detail {

/** @brief 合并清单 + 组件 bean 契约校验；返回 type_list 的反射 */
template <class App>
consteval std::meta::info app_beans_info() {
    using closure = component_closure_t<App>;
    validate_components_impl<closure>::run();
    using merged = unique_t<concat_t<
        auto_beans_t<App>,
        component_beans_t<closure>>>;
    return list_info<merged>::make();
}

} // namespace detail

/** @brief 应用最终 bean 清单：自动扫描 bean + 组件 bean，按类型去重 */
export template <class App>
using app_beans_t = [: detail::app_beans_info<App>() :];

/** @brief 应用最终 hooks 清单：组件闭包，按类型去重 */
export template <class App>
using app_hooks_t = unique_t<
    component_hooks_t<component_closure_t<App>>>;

// ==================== 启动器 ====================

template <class App, class... As>
int boot_via(unsigned short port, std::size_t workers, server_io io,
             std::size_t max_conns, const config::properties* cfg, As... aspects) {
    static_assert(detail::components_valid<app_components_t<App>>::value,
                  "cboot: App::components must be a type_list of Components");
    // 扫描/闭包标签依赖调用方类型（App）：实例化推迟到调用点（见模块头纪律）
    return detail::boot_runner<app_beans_t<App>, app_hooks_t<App>>::boot(
        port, workers, io, max_conns, cfg, std::move(aspects)...);
}

template <class... As>
int boot_auto(unsigned short port, As... aspects) {
    return boot_via<std::tuple<As...>>(port, 0, server_io::threads, 0, nullptr,
                                       std::move(aspects)...);
}

/** @brief 从配置读取 server.workers（缺省 0 = hardware_concurrency） */
inline std::size_t workers_from(const config::properties* cfg) {
    if (!cfg) return 0;
    if (auto v = cfg->get_expanded("server.workers")) {
        try {
            return static_cast<std::size_t>(std::stoul(std::string(*v)));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

/** @brief 读取 JSON 配置文件（失败带文件名与偏移；文件须为对象） */
inline config::properties load_config_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cboot: cannot open config file: " + path);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    try {
        return config::properties::from_json(text);
    } catch (const std::exception& e) {
        throw std::runtime_error("cboot: config '" + path + "': " + e.what());
    }
}

/**
 * @brief 应用基类（CRTP）— 应用身份即扫描锚点与配置载体
 *
 *   struct ShopApp : application<ShopApp> {
 *       static constexpr std::string_view config_path = "app.json"; // 可选
 *       static constexpr unsigned short default_port = 8080;        // 可选
 *       static constexpr auto aspects = std::tuple<RequestLog>{};   // 可选
 *   };
 *   int main() { return ShopApp{}.run(); }
 *
 * 配置（JSON，替代 Spring Boot 的 YAML）：
 *   - config_path（静态，可选）/ .config(path)（运行时覆盖）指定 JSON 文件
 *   - 文件为对象；嵌套展平为 "a.b" 键；${ENV:default} 占位符展开
 *   - bean 可在构造器注入 `const config::properties&` 读取
 *   - 端口优先级：.port(n) > 配置 server.port > default_port > 8080
 *
 * 其他语义：
 *   - App 类型本身是自动发现锚点（保证在调用方 TU 实例化）
 *   - aspects 可选（requires 检测）；.run() 阻塞服务
 */
export template <class Derived>
class application {
public:
    application() = default;
    application(const application&) = delete;
    application& operator=(const application&) = delete;

    /** @brief 运行时端口覆盖（0 = 配置/默认端口） */
    Derived& port(unsigned short p) {
        port_ = p;
        return self();
    }

    /** @brief 运行时配置路径覆盖（优先于 Derived::config_path） */
    Derived& config(std::string path) {
        config_path_ = std::move(path);
        return self();
    }

    /** @brief 运行时工作线程数覆盖（0 = 配置 server.workers / hardware_concurrency） */
    Derived& workers(std::size_t n) {
        workers_ = n;
        return self();
    }

    /** @brief 启动（无命令行参数） */
    int run() {
        return run(0, nullptr);
    }

    /**
     * @brief 启动并解析命令行/环境变量配置
     *
     * 优先级：
     *   - 配置路径：--config > .config(path) > Derived::config_path
     *   - 键值：CLI --key=value > CB_ 环境变量 > JSON 文件
     *   - 端口：.port(n) > 合并后的 server.port > default_port > 8080
     *   - worker：.workers(n) > 合并后的 server.workers
     */
    int run(int argc, char** argv) {
        // 1) CLI 与 CB_ 环境变量
        auto cli = config::parse_command_line(argc, argv);
        auto env = config::environment("CB_");

        // 2) 配置路径：CLI > 运行时 > 静态
        std::string path = cli.config_path;
        if (path.empty()) path = config_path_;
        if (path.empty()) {
            if constexpr (requires { Derived::config_path; })
                path.assign(Derived::config_path);
        }

        // 3) 合并：JSON → env → CLI
        config::properties merged;
        bool has_config = false;
        if (!path.empty()) {
            merged = load_config_file(path);
            has_config = true;
        }
        if (!env.empty()) {
            merged.overlay(env);
            has_config = true;
        }
        if (!cli.values.empty()) {
            merged.overlay(cli.values);
            has_config = true;
        }
        const config::properties* cfg = has_config ? &merged : nullptr;

        // 4) 端口：显式 > server.port > default_port > 8080
        unsigned short p = port_;
        if (p == 0 && cfg) {
            if (auto v = cfg->get_expanded("server.port"))
                p = static_cast<unsigned short>(std::stoi(std::string(*v)));
        }
        if (p == 0) {
            if constexpr (requires { Derived::default_port; }) p = Derived::default_port;
            else p = 8080;
        }

        // 5) 工作线程：显式 > server.workers > hardware_concurrency
        std::size_t w = workers_ != 0 ? workers_ : workers_from(cfg);

        // 6) 后端与背压
        server_io io = server_io::threads;
        std::size_t max_conns = 0;
        if (cfg) {
            if (auto v = cfg->get_expanded("server.io")) {
                const std::string s(*v);
                if (s == "iocp") io = server_io::iocp;
                else if (s == "epoll") io = server_io::epoll;
            }
            if (auto v = cfg->get_expanded("server.max_connections")) {
                try {
                    max_conns = static_cast<std::size_t>(std::stoul(std::string(*v)));
                } catch (...) {}
            }
        }

        if constexpr (requires { Derived::aspects; }) {
            return std::apply(
                [&](auto... as) { return boot_via<Derived>(p, w, io, max_conns, cfg, as...); },
                Derived::aspects);
        } else {
            return boot_via<Derived>(p, w, io, max_conns, cfg);
        }
    }

private:
    Derived& self() { return static_cast<Derived&>(*this); }
    unsigned short port_ = 0;
    std::size_t workers_ = 0;
    std::string config_path_;
};

/**
 * @brief Boot 引导糖 — 注解即注册（零清单；最小形态）
 *
 *   int main() { return run(8080); }
 *
 * 推荐 application<App> 形态（身份/配置集中）；本函数适用于最小脚本。
 * 逃生口：显式清单 run<Beans...>(port)（精确控制 bean 集合）。
 */
export template <class... As>
int run(unsigned short port = 8080, As... aspects) {
    return boot_auto(port, std::move(aspects)...);
}

/**
 * @brief Boot 引导糖（显式清单）— 精确控制 bean 集合（无自动发现）
 *
 *   int main() { return run<UserApi, UserService>(8080); }
 *
 * 流程：构造 DI 容器（编译期拓扑序）→ mount 全部 bean 路由 → 阻塞服务；
 * 退出时逆拓扑序 PreDestroy。
 */
export template <class B1, class... Bs>
int run(unsigned short port = 8080) {
    context<B1, Bs...> ctx(nullptr);
    server s(port);
    s.mount(ctx.template get<B1>());
    (s.mount(ctx.template get<Bs>()), ...);
    return s.run();
}
