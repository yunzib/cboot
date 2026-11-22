export module cboot.app;

import std;

// ── 对外的模块面：re-export（消费方仅需 `import cboot.app;`）──
// 注：CMake 的导入模块集不向消费方 TU 传递——re-export 让 app 成为
// 单一入口模块（import 一个即得全部框架词汇）。
export import cboot.aop;                // call_info / AspectFor / aspect_chain
export import cboot.config;             // config::properties / bind
export import cboot.core.annotations;   // Bean / Inject / Config / 生命周期
export import cboot.core.context;       // context / context_for
export import cboot.json;               // json::value / parse / from
export import cboot.meta.reflect;       // for_each_member / type_list / type_of_t
export import cboot.web.http;           // Get/Post/Put/Delete/Patch / Request / Response
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

/** @brief 清单执行器：容器构造（编译期拓扑序）→ 挂载路由（可带切面）→ 服务 */
template <class L>
struct boot_runner;
template <class... Ts>
struct boot_runner<type_list<Ts...>> {
    template <class... As>
    static int boot(unsigned short port, std::size_t workers, server_io io,
                    std::size_t max_conns, const config::properties* cfg,
                    As... aspects) {
        context<Ts...> ctx(cfg);                  // 先构造：先于 server 启动
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
        server s(sc);
        install_stop_handler(s);                  // Ctrl+C / 关闭 → 优雅停机
        // 按清单下标挂载：同基类的多个 bean 用类型 get 会歧义；
        // 未激活 profile 的 bean 不挂载。
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([&] {
                if (ctx.template is_active<Is>())
                    s.mount(ctx.template get_at<Is>(), aspects...);
            }(), ...);
        }(std::make_index_sequence<sizeof...(Ts)>{});
        int rc = s.run();
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

// ==================== 启动器 ====================

template <class App, class... As>
int boot_via(unsigned short port, std::size_t workers, server_io io,
             std::size_t max_conns, const config::properties* cfg, As... aspects) {
    // 扫描标签依赖调用方类型（App）：实例化推迟到调用点（见模块头纪律）
    return detail::boot_runner<auto_beans_t<App>>::boot(
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

    /** @brief 启动（阻塞直到停止；Ctrl+C/关闭事件 → 优雅停机） */
    int run() {
        // 1) 配置加载（路径：运行时 > 静态；未指定则不加载）
        std::string path = config_path_;
        if (path.empty()) {
            if constexpr (requires { Derived::config_path; })
                path.assign(Derived::config_path);
        }
        std::optional<config::properties> props;
        if (!path.empty()) props = load_config_file(path);
        const config::properties* cfg = props ? &*props : nullptr;

        // 2) 端口：显式 > server.port > default_port > 8080
        unsigned short p = port_;
        if (p == 0 && cfg) {
            if (auto v = cfg->get_expanded("server.port"))
                p = static_cast<unsigned short>(std::stoi(std::string(*v)));
        }
        if (p == 0) {
            if constexpr (requires { Derived::default_port; }) p = Derived::default_port;
            else p = 8080;
        }

        // 3) 工作线程：显式 > server.workers > hardware_concurrency
        std::size_t w = workers_ != 0 ? workers_ : workers_from(cfg);

        // 4) 后端与背压（配置内联解析；不引入非导出 helper，因模板在消费方实例化）
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
