export module cboot.web.routing;

import std;
import cboot.aop;            // call_info / aspect_chain（编译期切面）
import cboot.async;          // task<Response> handler（协程请求路径）
import cboot.core.annotations; // bean_name（路由中间件标签）
import cboot.json;
import cboot.meta.reflect;    // members_of / annotation_of / type_of_t（全局工具）
import cboot.validation;      // body DTO 字段校验
import cboot.web.http;
import cboot.web.middleware;  // request_context / middleware_fn / UseMiddleware

/**
 * @brief 路由 — 编译期路由表（注解提取）+ 运行时匹配调度
 *
 * 编译期（consteval）：
 *   - route_table<T>()：扫描 T 的成员函数，提取 [[=Get]]/[[=Post]] 注解
 *     → constexpr 数组（method / pattern / handler info）
 *   - 参数分类（Request / request_context / body DTO / 路径参数）
 *   - 路由级中间件标签 [[=UseMiddleware{"name"}]]
 *   - 路由级切面选择 [[=UseAspect{"name"}]] → 注册表中的切面 Bean
 * 运行时：
 *   - router：模板串匹配（"{x}" 段收参数）→ middleware 链 → handler 调用
 *
 * handler 参数协议（按声明序）：
 *   Request（或 const Request&）→ 请求本体（≤1 个）
 *   request_context（或 const&）→ 请求上下文（≤1 个）
 *   聚合体                     → JSON body 反序列化（≤1 个，POST）
 *   数值 / string / string_view → 路径参数（按 {x} 出现序消费）
 * 返回值：Response
 */

export namespace web {

/** @brief 切面角色注解；name 供 [[=web::UseAspect{"name"}]] 选择（须同时是 Bean） */
struct Aspect {
    bean_name name{};
};

/** @brief 路由级切面标签；handler 可重复标注，也可标在控制器类型上 */
struct UseAspect {
    bean_name name{};
};

/**
 * @brief 切面注册表协议 — 名称 → 切面 Bean 类型 / 实例
 *
 * 应用层提供（application 引导与 mount_routes 用容器实现）：
 *   using is_aspect_registry = void;                 // 重载消歧标记
 *   template <bean_name N> using aspect_type = ...;  // 名 → 切面类型；未注册 → 编译错
 *   template <class A> A& instance();                // 取切面实例（容器单例）
 */
template <class Registry>
concept AspectRegistry = requires {
    typename Registry::is_aspect_registry;
    { std::declval<Registry&>().template instance<int>() } -> std::same_as<int&>;
};

} // namespace web

// ── 内部工具（模块作用域，不导出）──────────────────────────────────────
namespace detail {

// 参数类型剥离 cv/引用（GCC16：substitute 产出别名形态，须 dealias 收尾）
consteval std::meta::info strip_type(std::meta::info t) {
    return std::meta::dealias(
        std::meta::substitute(^^std::remove_cvref_t, {t}));
}

template <std::meta::info t>
consteval bool is_path_param_type() {
    using P = type_of_t<t>;
    return (std::is_integral_v<P> && !std::is_same_v<P, bool>)
        || std::is_floating_point_v<P>
        || std::is_same_v<P, std::string>
        || std::is_same_v<P, std::string_view>;
}

/** @brief 路由条目（constexpr 数组元素；handler 供编译期模板实参用） */
struct route_entry {
    http_method method;
    path_name pattern;
    std::meta::info handler;
};

consteval bool is_route_fn(std::meta::info m) {
    return std::meta::is_function(m) && std::meta::is_class_member(m)
        && !std::meta::is_static_member(m)
        && (std::meta::annotations_of_with_type(m, ^^Get).size() > 0
            || std::meta::annotations_of_with_type(m, ^^Post).size() > 0
            || std::meta::annotations_of_with_type(m, ^^Put).size() > 0
            || std::meta::annotations_of_with_type(m, ^^Delete).size() > 0
            || std::meta::annotations_of_with_type(m, ^^Patch).size() > 0);
}

/** @brief 方法注解 → (method, path)；无方法注解返回 nullopt */
consteval std::optional<std::pair<http_method, path_name>>
route_annotation_of(std::meta::info m) {
    if (auto g = annotation_of<Get>(m))    return std::pair{http_method::get,   g->path};
    if (auto p = annotation_of<Post>(m))   return std::pair{http_method::post,  p->path};
    if (auto u = annotation_of<Put>(m))    return std::pair{http_method::put,   u->path};
    if (auto d = annotation_of<Delete>(m)) return std::pair{http_method::del,   d->path};
    if (auto a = annotation_of<Patch>(m))  return std::pair{http_method::patch, a->path};
    return std::nullopt;
}

template <class T>
consteval std::size_t route_count() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
        if (is_route_fn(m)) ++n;
    return n;
}

template <class T, std::size_t I>
consteval route_entry nth_route() {
    std::size_t seen = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (!is_route_fn(m)) continue;
        if (seen == I) {
            // 私有标注须报"必须 public"（GCC16.2：splice 调用私有成员
            // 报 "is private within this context"），而非静默丢路由
            if (!std::meta::is_public(m))
                throw "cboot.web: route handler must be public";
            if (auto r = route_annotation_of(m)) return {r->first, r->second, m};
        }
        ++seen;
    }
    throw "cboot.web: route index out of range";
}

// 单值 consteval 访问器（core nth_dep 同款：整调用为常量，不在临时上做访问）
template <class T, std::size_t I>
consteval std::meta::info nth_handler() { return nth_route<T, I>().handler; }

template <class T, std::size_t I>
consteval http_method nth_method() { return nth_route<T, I>().method; }

template <class T, std::size_t I>
consteval path_name nth_path_name() { return nth_route<T, I>().pattern; }

// ── 切面元数据：bean 名 / 方法名（identifier_of 静态存储，字符串视图常驻）──
template <class T>
consteval std::string_view type_ident() {
    if (std::meta::has_identifier(^^T))
        return std::meta::identifier_of(^^T);
    return {};
}

template <class T, std::size_t I>
consteval std::string_view handler_ident() {
    constexpr std::meta::info h = nth_handler<T, I>();
    if (std::meta::has_identifier(h))
        return std::meta::identifier_of(h);
    return {};
}

template <class T, std::size_t I>
consteval call_info call_info_for() {
    return call_info{type_ident<T>(), handler_ident<T, I>()};
}

// ── 路由级中间件标签 ──────────────────────────────────────────────────

template <class T, std::size_t I>
consteval std::size_t middleware_tag_count() {
    std::size_t n = 0;
    for (auto a : std::meta::annotations_of_with_type(
             nth_handler<T, I>(), ^^web::UseMiddleware))
        ++n;
    return n;
}

template <class T, std::size_t I, std::size_t K>
consteval bean_name middleware_tag_name() {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(
             nth_handler<T, I>(), ^^web::UseMiddleware)) {
        if (seen == K) return std::meta::extract<web::UseMiddleware>(a).name;
        ++seen;
    }
    return {};
}

template <class T, std::size_t I>
inline std::vector<std::string> route_tags() {
    std::vector<std::string> out;
    [&]<std::size_t... K>(std::index_sequence<K...>) {
        (out.emplace_back(middleware_tag_name<T, I, K>().view()), ...);
    }(std::make_index_sequence<middleware_tag_count<T, I>()>{});
    return out;
}

// ── handler 参数绑定 ────────────────────────────────────────────────────

template <std::meta::info Fn>
consteval std::size_t param_count() {
    return std::meta::parameters_of(Fn).size();
}

template <std::meta::info Fn>
consteval auto param_types_array() {
    constexpr std::size_t N = param_count<Fn>();
    std::array<std::meta::info, N> out{};
    for (std::size_t j = 0; j < N; ++j)
        out[j] = strip_type(std::meta::type_of(std::meta::parameters_of(Fn)[j]));
    return out;
}

/** @brief 第 J 参数消费第几个路径段（非路径参数 → npos） */
template <std::meta::info Fn, std::size_t J>
consteval std::size_t path_slot() {
    constexpr auto pts = param_types_array<Fn>();
    std::size_t k = 0;
    [&]<std::size_t... K>(std::index_sequence<K...>) {
        ((K < J && is_path_param_type<pts[K]>() ? (void)++k : void()), ...);
    }(std::make_index_sequence<J>{});
    return is_path_param_type<pts[J]>() ? k : std::size_t(-1);
}

/** @brief 构造第 J 个实参（Request / request_context / body DTO / 路径参数） */
template <std::meta::info Fn, std::size_t J>
decltype(auto) make_arg(web::request_context& ctx,
                        const std::vector<std::string_view>& pp) {
    constexpr std::meta::info pt = param_types_array<Fn>()[J];
    using P = type_of_t<pt>;
    if constexpr (pt == ^^Request) {
        return ctx.request;    // const Request& / Request（按值拷贝）皆可绑定
    } else if constexpr (pt == ^^web::request_context) {
        return ctx;
    } else if constexpr (std::is_aggregate_v<P> && !std::is_array_v<P>) {
        // body DTO：JSON 解析/类型错误与字段校验失败都归入 400。
        try {
            P dto = json::get_as<P>(json::parse(ctx.request.body));
            validation::validate_or_throw(dto);
            return dto;
        } catch (const validation::invalid&) {
            throw;
        } catch (const std::runtime_error& e) {
            throw validation::invalid({{"body", e.what()}});
        }
    } else {
        constexpr std::size_t slot = path_slot<Fn, J>();
        if constexpr (std::is_same_v<P, std::string>) {
            return std::string(pp[slot]);
        } else if constexpr (std::is_same_v<P, std::string_view>) {
            return pp[slot];
        } else {    // 数值路径参数
            P v{};
            auto [p, ec] = std::from_chars(pp[slot].data(),
                                           pp[slot].data() + pp[slot].size(), v);
            if (ec != std::errc{})
                throw std::runtime_error("cboot.web: bad path parameter");
            return v;
        }
    }
}

/** @brief handler 调用（成员函数 splice + 参数按位构造） */
template <class T, std::meta::info Fn>
Response invoke(T& obj, web::request_context& ctx,
                const std::vector<std::string_view>& pp) {
    constexpr std::size_t N = param_count<Fn>();
    return [&]<std::size_t... J>(std::index_sequence<J...>) -> Response {
        auto&& r = (obj.[:Fn:])(make_arg<Fn, J>(ctx, pp)...);
        // 协程 handler：task<Response>（可 co_await 其他 task；I/O 级 await 后续）
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(r)>, task<Response>>) {
            return sync_wait(std::move(r));
        } else {
            return r;
        }
    }(std::make_index_sequence<N>{});
}

// handler 工厂：Fn 为模板实参（consteval 调用形态），lambda 无捕获负担
using handler_fn = std::function<Response(web::request_context&,
                                          const std::vector<std::string_view>&)>;

template <class T, std::meta::info Fn>
handler_fn make_handler(T& obj) {
    return [&obj](web::request_context& ctx,
                  const std::vector<std::string_view>& pp) {
        return invoke<T, Fn>(obj, ctx, pp);
    };
}

        } // namespace detail

// ── 方法级切面选择：[[=web::UseAspect{"name"}]]（与 UseMiddleware 同构）──────
// 命名空间 detail 继续：单值 consteval 访问器（GCC16 纪律：不在 consteval
// 临时上做成员访问 / 模板实参）。

namespace detail {

/** @brief 控制器类型上的 [[=web::UseAspect]] 数量（类级：对全部路由生效） */
template <class T>
consteval std::size_t class_aspect_count() {
    std::size_t n = 0;
    for (auto a : std::meta::annotations_of_with_type(^^T, ^^web::UseAspect))
        ++n;
    return n;
}

template <class T>
consteval bean_name class_aspect_at(std::size_t k) {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(^^T, ^^web::UseAspect)) {
        if (seen == k) return std::meta::extract<web::UseAspect>(a).name;
        ++seen;
    }
    return {};
}

/** @brief 路由 I 的 handler 上的 [[=web::UseAspect]] 数量（方法级：追加） */
template <class T, std::size_t I>
consteval std::size_t method_aspect_count() {
    std::size_t n = 0;
    for (auto a : std::meta::annotations_of_with_type(
             nth_handler<T, I>(), ^^web::UseAspect))
        ++n;
    return n;
}

template <class T, std::size_t I>
consteval bean_name method_aspect_at(std::size_t k) {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(
             nth_handler<T, I>(), ^^web::UseAspect)) {
        if (seen == k) return std::meta::extract<web::UseAspect>(a).name;
        ++seen;
    }
    return {};
}

/** @brief 第 j 个候选名：类级在前，方法级追加 */
template <class T, std::size_t I>
consteval bean_name aspect_candidate_at(std::size_t j) {
    if (j < class_aspect_count<T>()) return class_aspect_at<T>(j);
    return method_aspect_at<T, I>(j - class_aspect_count<T>());
}

/** @brief 第 j 个候选是否与更早的候选重名（重复标注只生效一次） */
template <class T, std::size_t I>
consteval bool aspect_candidate_duplicated(std::size_t j) {
    const bean_name name = aspect_candidate_at<T, I>(j);
    for (std::size_t p = 0; p < j; ++p)
        if (aspect_candidate_at<T, I>(p) == name) return true;
    return false;
}

template <class T, std::size_t I>
consteval std::size_t aspect_candidate_total() {
    return class_aspect_count<T>() + method_aspect_count<T, I>();
}

/** @brief 去重后的切面选择数（类级在前、方法级追加） */
template <class T, std::size_t I>
consteval std::size_t selected_aspect_count() {
    std::size_t n = 0;
    const std::size_t total = aspect_candidate_total<T, I>();
    for (std::size_t j = 0; j < total; ++j)
        if (!aspect_candidate_duplicated<T, I>(j)) ++n;
    return n;
}

/** @brief 路由 I 去重后的第 K 个切面名；必须非空，否则编译错 */
template <class T, std::size_t I, std::size_t K>
consteval bean_name selected_aspect_name() {
    std::size_t n = 0;
    const std::size_t total = aspect_candidate_total<T, I>();
    for (std::size_t j = 0; j < total; ++j) {
        if (aspect_candidate_duplicated<T, I>(j)) continue;
        if (n == K) {
            const bean_name name = aspect_candidate_at<T, I>(j);
            if (name.empty())
                throw "cboot.web: [[=web::UseAspect{}]] requires a name";
            return name;
        }
        ++n;
    }
    return {};
}

/**
 * @brief 路由 I 的第 K 个选中切面对应的 Bean 类型
 *
 * GCC16：模板实参里不能嵌 f<T, I>(k) 形态（'>' 被提前当作闭合），序号走
 * 模板参数 + 单值 consteval 访问器。
 */
template <class Registry, class T, std::size_t I, std::size_t K>
using selected_aspect_type_for =
    typename Registry::template aspect_type<selected_aspect_name<T, I, K>()>;

/** @brief T 是否用了 [[=web::UseAspect]]（类级或任一路由方法） */
template <class T, std::size_t I = 0>
consteval bool any_use_aspect() {
    if constexpr (I < route_count<T>()) {
        if (method_aspect_count<T, I>() > 0) return true;
        return any_use_aspect<T, I + 1>();
    } else {
        return class_aspect_count<T>() > 0;
    }
}

/**
 * @brief 路由 I 的 handler 反射（类型形态）
 *
 * GCC16：三层 lambda 内写 invoke<T, nth_handler<T, I>()>(...) 解析失败
 * （模板实参里不能嵌 consteval 调用）→ 经类型里的常量传模板实参。
 */
template <class T, std::size_t I>
struct handler_ref {
    static constexpr std::meta::info fn = nth_handler<T, I>();
};

/**
 * @brief 按路由 I 的选择结果构造内层切面链
 *
 * 链类型 aspect_chain<Selected&...>：选中的切面 Bean 以引用进入链（共享
 * 容器实例）。全局切面不在这里——它们由 router 为每个 bean 建一条共享的
 * 外层链，保证"全局切面状态在 bean 的全部路由间共享"与无 UseAspect 时一致。
 */
template <class Registry, class Seq>
struct selected_chain_builder;

template <class Registry, std::size_t... Ks>
struct selected_chain_builder<Registry, std::index_sequence<Ks...>> {
    template <class T, std::size_t I>
    static auto build(Registry& registry) {
        using chain_type = aspect_chain<
            selected_aspect_type_for<Registry, T, I, Ks>&...>;
        return std::make_shared<chain_type>(
            registry.template instance<
                selected_aspect_type_for<Registry, T, I, Ks>>()...);
    }
};

} // namespace detail

/**
 * @brief 运行时路由器 — 模板匹配 + 调度（纯逻辑，无 socket；server 的内核）
 */
export class router {
public:
    /** @brief 挂载 bean：编译期路由表（nth_route 逐条递归注册） */
    template <class T>
    void mount(T& obj) {
        mount_one<T, 0>(obj);
    }

    /**
     * @brief 挂载 bean + 切面链（洋葱模型，As[0] 最外层）
     *
     *   r.mount(api, Trace{}, Auth{});   // 每个 handler 调用都经切面
     *
     * 切面在编译期展开为静态调用链（无代理、无虚调用），切面对象可有
     * 状态（计数器/限流窗口等），随 mount 复制进链。
     */
    template <class T, class... As>
        requires (sizeof...(As) > 0 && (!web::AspectRegistry<As> && ...))
    void mount(T& obj, As... aspects) {
        auto chain = std::make_shared<aspect_chain<As...>>(std::move(aspects)...);
        mount_with_chain<T, 0>(obj, std::move(chain));
    }

    /**
     * @brief 挂载 bean + 方法级切面选择
     *
     *   r.mount(api, registry, Trace{});   // Trace 为全局切面（最外层）
     *
     * handler 或控制器上的 [[=web::UseAspect{"name"}]] 按名从 registry 选择
     * 切面 Bean：类级对全部路由生效，方法级追加，重名只生效一次。未注册的
     * 名字 → 编译错。T 未用 UseAspect 时行为与 mount(obj, aspects...) 相同。
     */
    template <class T, web::AspectRegistry Registry, class... As>
    void mount(T& obj, Registry registry, As... aspects) {
        if constexpr (detail::any_use_aspect<T>()) {
            // 外层：全局切面，每个 bean 一条共享链（同 mount(obj, aspects...)）
            auto outer = std::make_shared<aspect_chain<As...>>(std::move(aspects)...);
            mount_selected<T, Registry, 0, As...>(obj, registry, std::move(outer));
        } else if constexpr (sizeof...(As) == 0) {
            mount_one<T, 0>(obj);              // 无切面：不建链
        } else {
            // 未用 UseAspect：沿用"每个 bean 一条共享链"
            auto chain = std::make_shared<aspect_chain<As...>>(std::move(aspects)...);
            mount_with_chain<T, 0>(obj, std::move(chain));
        }
    }

    /**
     * @brief 注册中间件（启动期调用）
     *
     * name 为空 = 全局；非空 = 仅匹配带 [[=UseMiddleware{"name"}]] 的路由。
     * order 小的在外层；同 order 按 name 稳定排序。
     */
    void use(std::string name, int order, web::middleware_fn fn);

    /** @brief 调度（404 未命中 / 405 方法不匹配；实现于 src/web/routing.cpp） */
    Response dispatch(std::string_view method, std::string_view target,
                      std::string_view body) const;
    Response dispatch(std::string_view method, std::string_view target,
                      std::string_view body, std::string_view headers) const;

    std::size_t size() const { return routes_.size(); }
    std::size_t middleware_count() const { return middlewares_.size(); }

private:
    // 递归注册（GCC16 纪律：consteval 临时上不做成员访问/模板实参——
    // 经单值 consteval 访问器取用，core nth_dep 同款）
    template <class T, std::size_t I>
    void mount_one(T& obj) {
        if constexpr (I < detail::route_count<T>()) {
            static_assert(detail::selected_aspect_count<T, I>() == 0,
                          "cboot.web: [[=web::UseAspect]] needs the registry "
                          "mount: mount(obj, registry, aspects...), application "
                          "boot, or mount_routes()");
            add_route(detail::nth_method<T, I>(),
                      detail::nth_path_name<T, I>().view(),
                      detail::make_handler<T, detail::nth_handler<T, I>()>(obj),
                      std::string(detail::type_ident<T>()),
                      std::string(detail::handler_ident<T, I>()),
                      detail::route_tags<T, I>());
            mount_one<T, I + 1>(obj);
        }
    }

    // 切面版注册：每 handler 经共享链包裹（call_info 名称来自反射）
    template <class T, std::size_t I, class Chain>
    void mount_with_chain(T& obj, std::shared_ptr<Chain> chain) {
        if constexpr (I < detail::route_count<T>()) {
            static_assert(detail::selected_aspect_count<T, I>() == 0,
                          "cboot.web: [[=web::UseAspect]] needs the registry "
                          "mount: mount(obj, registry, aspects...), application "
                          "boot, or mount_routes()");
            add_route(detail::nth_method<T, I>(),
                      detail::nth_path_name<T, I>().view(),
                      [&obj, chain](web::request_context& ctx,
                                    const std::vector<std::string_view>& pp) {
                          return chain->run(detail::call_info_for<T, I>(),
                                            [&]() -> Response {
                              return detail::invoke<T, detail::nth_handler<T, I>()>(
                                  obj, ctx, pp);
                          });
                      },
                      std::string(detail::type_ident<T>()),
                      std::string(detail::handler_ident<T, I>()),
                      detail::route_tags<T, I>());
            mount_with_chain<T, I + 1>(obj, std::move(chain));
        }
    }

    // 方法级选择版注册：外层链（全局切面）全 bean 共享；每条路由按自己的
    // [[=web::UseAspect]] 选择集建内层链（各路由选择集不同 → 链类型不同）
    template <class T, class Registry, std::size_t I, class... As>
    void mount_selected(T& obj, Registry& registry,
                        const std::shared_ptr<aspect_chain<As...>>& outer) {
        if constexpr (I < detail::route_count<T>()) {
            using builder = detail::selected_chain_builder<
                Registry,
                std::make_index_sequence<detail::selected_aspect_count<T, I>()>>;
            auto inner = builder::template build<T, I>(registry);
            add_route(detail::nth_method<T, I>(),
                      detail::nth_path_name<T, I>().view(),
                      [&obj, outer, inner,
                       ci = detail::call_info_for<T, I>()](
                          web::request_context& ctx,
                          const std::vector<std::string_view>& pp) {
                          return outer->run(ci, [&]() -> Response {
                              return inner->run(ci, [&]() -> Response {
                                  return detail::invoke<
                                      T, detail::handler_ref<T, I>::fn>(
                                      obj, ctx, pp);
                              });
                          });
                      },
                      std::string(detail::type_ident<T>()),
                      std::string(detail::handler_ident<T, I>()),
                      detail::route_tags<T, I>());
            mount_selected<T, Registry, I + 1, As...>(obj, registry, outer);
        }
    }

    struct route_item {
        http_method method;
        std::string pattern;
        std::vector<std::string> segs;    // 预切分（"{x}" 起始段为参数段）
        detail::handler_fn h;
        std::string bean;
        std::string handler;
        std::vector<std::string> middleware;
    };

    struct middleware_entry {
        std::string name;    // 空 = 全局
        int order = 0;
        web::middleware_fn fn;
    };

    void add_route(http_method method, std::string_view pattern,
                   detail::handler_fn h, std::string bean, std::string handler,
                   std::vector<std::string> middleware);

    template <class Core>
    Response run_chain(std::size_t index, web::request_context& ctx,
                       const Core& core) const {
        while (index < middlewares_.size()) {
            const auto& mw = middlewares_[index];
            bool applies = mw.name.empty();
            if (!applies && ctx.route) {
                applies = std::find(ctx.route->middleware_tags.begin(),
                                    ctx.route->middleware_tags.end(), mw.name)
                    != ctx.route->middleware_tags.end();
            }
            if (applies) break;
            ++index;
        }
        if (index == middlewares_.size()) return core();
        web::next_fn next = [this, index, &ctx, &core] {
            return run_chain(index + 1, ctx, core);
        };
        return middlewares_[index].fn(ctx, next);
    }

    std::vector<route_item> routes_;
    std::vector<middleware_entry> middlewares_;
};
