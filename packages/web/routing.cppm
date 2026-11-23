export module cboot.web.routing;

import std;
import cboot.aop;            // call_info / aspect_chain（编译期切面）
import cboot.async;          // task<Response> handler（协程请求路径）
import cboot.json;
import cboot.meta.reflect;    // members_of / annotation_of / type_of_t（全局工具）
import cboot.validation;      // body DTO 字段校验
import cboot.web.http;

/**
 * @brief 路由 — 编译期路由表（注解提取）+ 运行时匹配调度
 *
 * 编译期（consteval）：
 *   - route_table<T>()：扫描 T 的成员函数，提取 [[=Get]]/[[=Post]] 注解
 *     → constexpr 数组（method / pattern / handler info）
 *   - 参数分类（Request / body DTO / 路径参数）+ 路径参数槽位计算
 * 运行时：
 *   - router：模板串匹配（"{x}" 段收参数）→ handler 调用
 *
 * handler 参数协议（按声明序）：
 *   Request（或 const Request&）→ 请求本体（≤1 个）
 *   聚合体                     → JSON body 反序列化（≤1 个，POST）
 *   数值 / string / string_view → 路径参数（按 {x} 出现序消费）
 * 返回值：Response
 */

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
    return std::meta::identifier_of(^^T);
}

template <class T, std::size_t I>
consteval std::string_view handler_ident() {
    return std::meta::identifier_of(nth_handler<T, I>());
}

template <class T, std::size_t I>
consteval call_info call_info_for() {
    return call_info{type_ident<T>(), handler_ident<T, I>()};
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

/** @brief 构造第 J 个实参（Request / body DTO / 路径参数转换） */
template <std::meta::info Fn, std::size_t J>
auto make_arg(const Request& req, const std::vector<std::string_view>& pp) {
    constexpr std::meta::info pt = param_types_array<Fn>()[J];
    using P = type_of_t<pt>;
    if constexpr (pt == ^^Request) {
        return req;    // const Request& / Request（按值拷贝）皆可绑定
    } else if constexpr (std::is_aggregate_v<P> && !std::is_array_v<P>) {
        // body DTO：JSON 解析/类型错误与字段校验失败都归入 400。
        try {
            P dto = json::get_as<P>(json::parse(req.body));
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
Response invoke(T& obj, const Request& req, const std::vector<std::string_view>& pp) {
    constexpr std::size_t N = param_count<Fn>();
    return [&]<std::size_t... J>(std::index_sequence<J...>) -> Response {
        auto&& r = (obj.[:Fn:])(make_arg<Fn, J>(req, pp)...);
        // 协程 handler：task<Response>（可 co_await 其他 task；I/O 级 await 后续）
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(r)>, task<Response>>) {
            return sync_wait(std::move(r));
        } else {
            return r;
        }
    }(std::make_index_sequence<N>{});
}

// handler 工厂：Fn 为模板实参（consteval 调用形态），lambda 无捕获负担
using handler_fn = std::function<Response(const Request&,
                                          const std::vector<std::string_view>&)>;

template <class T, std::meta::info Fn>
handler_fn make_handler(T& obj) {
    return [&obj](const Request& req, const std::vector<std::string_view>& pp) {
        return invoke<T, Fn>(obj, req, pp);
    };
}

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
        requires (sizeof...(As) > 0)
    void mount(T& obj, As... aspects) {
        auto chain = std::make_shared<aspect_chain<As...>>(std::move(aspects)...);
        mount_with_chain<T, 0>(obj, std::move(chain));
    }

    /** @brief 调度（404 未命中 / 405 方法不匹配；实现于 src/web/routing.cpp） */
    Response dispatch(std::string_view method, std::string_view target,
                      std::string_view body) const;

    std::size_t size() const { return routes_.size(); }

private:
    // 递归注册（GCC16 纪律：consteval 临时上不做成员访问/模板实参——
    // 经单值 consteval 访问器取用，core nth_dep 同款）
    template <class T, std::size_t I>
    void mount_one(T& obj) {
        if constexpr (I < detail::route_count<T>()) {
            add_route(detail::nth_method<T, I>(),
                      detail::nth_path_name<T, I>().view(),
                      detail::make_handler<T, detail::nth_handler<T, I>()>(obj));
            mount_one<T, I + 1>(obj);
        }
    }

    // 切面版注册：每 handler 经共享链包裹（call_info 名称来自反射）
    template <class T, std::size_t I, class Chain>
    void mount_with_chain(T& obj, std::shared_ptr<Chain> chain) {
        if constexpr (I < detail::route_count<T>()) {
            add_route(detail::nth_method<T, I>(),
                      detail::nth_path_name<T, I>().view(),
                      [&obj, chain](const Request& req,
                                    const std::vector<std::string_view>& pp) {
                          return chain->run(detail::call_info_for<T, I>(),
                                            [&]() -> Response {
                              return detail::invoke<T, detail::nth_handler<T, I>()>(
                                  obj, req, pp);
                          });
                      });
            mount_with_chain<T, I + 1>(obj, std::move(chain));
        }
    }

    struct route_item {
        http_method method;
        std::vector<std::string> segs;    // 预切分（"{x}" 起始段为参数段）
        detail::handler_fn h;
    };

    void add_route(http_method method, std::string_view pattern,
                   detail::handler_fn h);
    const route_item* match(bool is_post, std::string_view path,
                            std::vector<std::string_view>& pp) const;

    std::vector<route_item> routes_;
};
