export module cboot.core.context;

import std;
import cboot.meta.reflect;      // find_annotation / annotation_of（全局作用域工具）
import cboot.config;            // config::properties（内建注入源）
import cboot.core.annotations;  // cboot::Inject / PostConstruct / PreDestroy ...

/**
 * @brief 依赖注入容器 — 编译期规划，运行时执行
 *
 * 职责切分（"把 Spring 运行时干的事搬到编译期"的落地）：
 *   编译期（consteval，错误在此暴露）：
 *     - 构造器选择（[[=cboot::Inject{}]] 标注 / 唯一候选 / 聚合默认构造）
 *     - 依赖提取（parameters_of → 参数类型精确匹配，含 cv/引用）
 *     - 依赖满足校验（缺失 → throw "unsatisfied dependency"）
 *     - Kahn 拓扑排序（环 → throw "circular dependency"）
 *     - 生命周期方法检索（[[=PostConstruct{}]] / [[=PreDestroy{}]]）
 *   运行时（微秒级）：
 *     - 按拓扑序 placement-new + 引用直连注入（零查表零间接）
 *     - 每 bean 构造后立即 PostConstruct（Spring 语义：初始化完成先于依赖者）
 *     - 析构：逆拓扑序 PreDestroy，随后内存自动释放
 *
 * GCC16.2 纪律继承：vector<info> 不外逃 / splice 操作数取常量表达式形态
 * （constexpr 局部或 consteval 调用）/ 构造调用走重载决议（传精确参数类型
 * → 自动选中被反射选定的构造器，无需 splice 构造器本体）。
 */
namespace detail {

/** @brief 内建依赖：config::properties（由 app 加载后注入，不算 bean） */
consteval bool is_config_dep(std::meta::info t) { return t == ^^config::properties; }

/** @brief 配置 bean 判据：类型带 [[=Config{...}]] 标注 */
consteval bool is_config_bean(std::meta::info T) {
    return std::meta::annotations_of_with_type(T, ^^Config).size() > 0;
}

/**
 * @brief 配置 bean 的绑定注解（静态存储：prefix 的字符串视图常驻）
 * 注：extract 的返回值是临时量，其字符数组生命周期随表达式——必须经
 * inline constexpr 变量模板固化为静态存储后再取 view()
 */
template <class T>
inline constexpr Config config_anno = []() consteval {
    for (auto a : std::meta::annotations_of_with_type(^^T, ^^Config))
        return std::meta::extract<Config>(a);
    return Config{};
}();

// ── 构造器枚举（用户声明；剔除拷贝/移动/删除）──────────────────────────

template <class T>
consteval bool is_bean_ctor(std::meta::info m) {
    return std::meta::is_constructor(m)
        && std::meta::is_user_declared(m)
        && !std::meta::is_deleted(m)
        && !std::meta::is_copy_constructor(m)
        && !std::meta::is_move_constructor(m);
}

template <class T>
consteval std::size_t ctor_count() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
        if (is_bean_ctor<T>(m)) ++n;
    return n;
}

template <class T, std::size_t I>
consteval std::meta::info nth_ctor() {
    std::size_t seen = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (is_bean_ctor<T>(m)) {
            if (seen == I) return m;
            ++seen;
        }
    }
    throw "cboot: ctor index out of range";
}

/**
 * @brief 构造器选择（Spring 语义子集）
 *   1. 恰一个 [[=cboot::Inject{}]] 标注构造器 → 选它（多个 → 编译错）
 *   2. 无标注 + 恰一个候选 → 选它
 *   3. 无候选（聚合/隐式默认构造）→ nullopt，默认构造
 *   4. 多候选无标注 → 编译错（要求显式 [[=cboot::Inject{}]]）
 *
 * 访问控制（GCC16.2 实测边界）：私有构造器反射可见（unchecked），但
 * "cannot use constructor in a splice expression"——框架构造须经重载
 * 决议，故构造器必须 public；此处给出明确诊断而非底层 access 错误。
 */
template <class T>
consteval std::optional<std::meta::info> select_ctor() {
    if (is_config_bean(^^T)) return std::nullopt;    // 配置 bean 由 bind 生成
    std::optional<std::meta::info> annotated;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (!is_bean_ctor<T>(m)) continue;
        if (find_annotation<Inject>(m)) {
            if (annotated) throw "cboot: multiple [[=Inject{}]] constructors";
            annotated = m;
        }
    }
    if (annotated) {
        if (!std::meta::is_public(*annotated))
            throw "cboot: [[=Inject{}]] constructor must be public";
        return annotated;
    }
    constexpr std::size_t total = ctor_count<T>();
    if (total == 0) return std::nullopt;
    if (total == 1) {
        if (!std::meta::is_public(nth_ctor<T, 0>()))
            throw "cboot: bean constructor must be public";
        return nth_ctor<T, 0>();
    }
    throw std::define_static_string(
        std::string("cboot: '") + std::string(std::meta::identifier_of(^^T))
        + "' has " + std::to_string(total)
        + " constructors — annotate the injected one with [[=Inject{}]]");
}

// ── 依赖提取 ────────────────────────────────────────────────────────────

/** @brief 剥离依赖参数类型的 cv/引用（substitute 实例化 remove_cvref_t）
 * dealias 收尾：substitute 产出别名实例化形态，与裸类 ^^B 比较需先去别名 */
consteval std::meta::info strip_dep(std::meta::info t) {
    return std::meta::dealias(std::meta::substitute(^^std::remove_cvref_t, {t}));
}



/** @brief 依赖数 = 选定构造器的参数数 */
template <class T>
consteval std::size_t dep_count() {
    constexpr auto c = select_ctor<T>();
    if constexpr (c.has_value()) return std::meta::parameters_of(*c).size();
    else return 0;
}

/**
 * @brief 第 J 个依赖的 bean 类型反射（已剥 cv/引用）
 * 清单匹配用剥后形态；构造实参仍传 bean 左值，重载决议按原参数类型绑定
 */
template <class T, std::size_t J>
consteval std::meta::info nth_dep() {
    constexpr auto c = select_ctor<T>();
    return strip_dep(std::meta::type_of(std::meta::parameters_of(*c)[J]));
}

/** @brief d(info) 在 Bs 清单中的下标；未命中 throw（缺依赖诊断点） */
template <std::meta::info d, class... Bs>
consteval std::size_t bean_index() {
    std::array<std::meta::info, sizeof...(Bs)> types{^^Bs...};
    for (std::size_t i = 0; i < types.size(); ++i)
        if (types[i] == d) return i;
    throw "cboot: unsatisfied dependency (type not in bean list)";
}

/** @brief bean A 是否依赖 bean B（参数类型剥 cv/引用后与 ^^B 匹配） */
template <class A, class B>
consteval bool depends_on() {
    constexpr auto c = select_ctor<A>();
    if constexpr (c.has_value()) {
        for (auto p : std::meta::parameters_of(*c))
            if (strip_dep(std::meta::type_of(p)) == ^^B) return true;
    }
    return false;
}

// ── 依赖矩阵（I×J 笛卡尔展开；pack 索引 Bs...[I] 为 C++26 特性）────────
// 注意两条 GCC16 折叠纪律：
//   1. fold 展开模式内"全部" pack（双 pack 会被 zip 而非笛卡尔）——J 经
//      index_sequence lambda 隔离为内层单 fold
//   2. 外层 fold 的模式若为 lambda 调用、pack 仅现于 lambda 体内，GCC 判
//      "模式无未展开 pack"——故外层不走 fold，改递归填行

template <std::size_t I, class... Bs>
consteval void fill_deps_row(
    std::array<std::size_t, sizeof...(Bs) * sizeof...(Bs)>& deps) {
    constexpr std::size_t N = sizeof...(Bs);
    if constexpr (I < N) {
        [&]<std::size_t... J>(std::index_sequence<J...>) {
            ((deps[I * N + J] = depends_on<Bs...[I], Bs...[J]>()), ...);
        }(std::make_index_sequence<N>{});
        fill_deps_row<I + 1, Bs...>(deps);
    }
}

template <class... Bs>
consteval auto build_deps() {
    constexpr std::size_t N = sizeof...(Bs);
    std::array<std::size_t, N * N> deps{};
    fill_deps_row<0, Bs...>(deps);
    return deps;
}

/** @brief 依赖满足校验（带类型名的诊断：'A' requires 'B'） */
template <class T, std::meta::info d, class... Bs>
consteval void check_dep_named() {
    if (is_config_dep(d)) return;
    std::array<std::meta::info, sizeof...(Bs)> types{^^Bs...};
    for (auto t : types)
        if (t == d) return;
    throw std::define_static_string(
        std::string("cboot: '") + std::string(std::meta::identifier_of(^^T))
        + "' requires '" + std::string(std::meta::identifier_of(d))
        + "' which is not in the bean list (annotate it [[=Bean{}]] "
          "or add it to run<...>)");
}

template <class T, class... Bs>
consteval void validate_deps() {
    constexpr std::size_t D = dep_count<T>();
    [&]<std::size_t... J>(std::index_sequence<J...>) {
        (check_dep_named<T, nth_dep<T, J>(), Bs...>(), ...);
    }(std::make_index_sequence<D>{});
}

// ── 环路径诊断（DFS 找回边，输出 "A -> B -> A"）────────────────────────

template <std::size_t N>
struct cycle_finder {
    const std::array<std::size_t, N * N>& deps;
    const std::array<std::meta::info, N>& types;
    std::array<int, N> state{};
    std::array<std::size_t, N> stack{};
    std::size_t depth = 0;
    std::string msg;

    consteval bool dfs(std::size_t u) {
        state[u] = 1;                 // 栈中
        stack[depth++] = u;
        for (std::size_t v = 0; v < N; ++v) {
            if (!deps[u * N + v]) continue;
            if (state[v] == 0) {
                if (dfs(v)) return true;
            } else if (state[v] == 1) {    // 回边 → 环
                std::size_t start = 0;
                while (start < depth && stack[start] != v) ++start;
                msg = "cboot: circular dependency: ";
                for (std::size_t i = start; i < depth; ++i) {
                    msg += std::meta::identifier_of(types[stack[i]]);
                    msg += " -> ";
                }
                msg += std::meta::identifier_of(types[v]);
                return true;
            }
        }
        --depth;
        state[u] = 2;                 // 完成
        return false;
    }
};

template <class... Bs>
consteval std::string find_cycle(
    const std::array<std::size_t, sizeof...(Bs) * sizeof...(Bs)>& deps) {
    constexpr std::size_t N = sizeof...(Bs);
    std::array<std::meta::info, N> types{^^Bs...};
    cycle_finder<N> f{deps, types};
    for (std::size_t u = 0; u < N; ++u)
        if (f.state[u] == 0 && f.dfs(u)) return f.msg;
    return "cboot: circular dependency detected";
}

/**
 * @brief Kahn 拓扑排序（consteval）
 * @return 构造顺序（清单下标数组）；环 → throw "circular dependency"
 * 确定性：同层取清单序（首个 indeg=0 者）
 */
template <class... Bs>
consteval auto topo_order() {
    constexpr std::size_t N = sizeof...(Bs);
    (validate_deps<Bs, Bs...>(), ...);

    std::array<std::size_t, N * N> deps = build_deps<Bs...>();

    std::array<std::size_t, N> indeg{};
    for (std::size_t i = 0; i < N; ++i) {
        std::size_t d = 0;
        for (std::size_t j = 0; j < N; ++j) d += deps[i * N + j];
        indeg[i] = d;
    }

    std::array<std::size_t, N> order{};
    std::array<bool, N> done{};
    for (std::size_t k = 0; k < N; ++k) {
        bool progressed = false;
        for (std::size_t i = 0; i < N; ++i) {
            if (!done[i] && indeg[i] == 0) {
                order[k] = i;
                done[i] = true;
                progressed = true;
                for (std::size_t d = 0; d < N; ++d)
                    if (deps[d * N + i]) --indeg[d];
                break;
            }
        }
        if (!progressed) throw std::define_static_string(find_cycle<Bs...>(deps));
    }
    return order;
}

// ── 生命周期方法检索 ────────────────────────────────────────────────────

/**
 * @brief 找 anno 注解的非静态成员函数（≤1 个）
 *
 * unchecked 扫描使其可见（含私有）——私有标注须报"必须 public"
 * （GCC16.2：splice 调用私有成员函数报 "is private within this context"），
 * 而不是静默忽略用户的注解。
 */
template <class T>
consteval std::meta::info lifecycle_fn(std::meta::info anno) {
    std::meta::info found{};
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (!std::meta::is_function(m) || !std::meta::is_class_member(m)
            || std::meta::is_static_member(m)) continue;
        if (std::meta::annotations_of_with_type(m, anno).size() > 0) {
            if (found != std::meta::info{}) throw "cboot: multiple lifecycle methods";
            if (!std::meta::is_public(m))
                throw "cboot: lifecycle method must be public";
            found = m;
        }
    }
    return found;
}

} // namespace detail

/**
 * @brief 依赖注入容器（单例域）
 *
 * 用法：
 *   cboot::context<UserRepository, AuditLog, UserService> ctx;
 *   ctx.get<UserService>().serve(42);
 *
 * Bean 编写约定（Phase 1）：
 *   - 构造器参数为依赖类型（引用或值，精确匹配清单内类型）
 *   - 多构造器时以 [[=cboot::Inject{}]] 标注注入用构造器
 *   - 公有构造器（反射访问上下文限制）
 */
export template <class... Beans>
class context {
public:
    /** @brief cfg：应用配置（nullptr = 无；bean 构造器注入 const config::properties&） */
    explicit context(const config::properties* cfg = nullptr) : cfg_(cfg) { build_all(); }
    ~context() { pre_destroy_all(); }
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    /** @brief 类型精确匹配取 bean（类型不在清单 → 编译错） */
    template <class T>
    T& get() {
        constexpr std::size_t I = detail::bean_index<^^T, Beans...>();
        auto& p = std::get<I>(beans_);
        if (!p) throw std::runtime_error("cboot: bean not constructed");
        return *p;
    }

private:
    template <std::size_t I>
    using bean_t = typename std::tuple_element_t<I,
        std::tuple<std::unique_ptr<Beans>...>>::element_type;

    std::tuple<std::unique_ptr<Beans>...> beans_{};
    const config::properties* cfg_ = nullptr;

    // ── 构造 + PostConstruct（拓扑序逐 bean）────────────────────────────

    void build_all() {
        constexpr auto order = detail::topo_order<Beans...>();
        for (std::size_t k = 0; k < order.size(); ++k) {
            construct_indexed(order[k]);
            post_construct_indexed(order[k]);
        }
    }

    template <std::size_t I = 0>
    void construct_indexed(std::size_t i) {
        if constexpr (I < sizeof...(Beans)) {
            if (i == I) construct_one<I>();
            else construct_indexed<I + 1>(i);
        }
    }

    template <std::size_t I>
    void construct_one() {
        construct_bean<bean_t<I>>();
    }

    /**
     * 构造分两路：
     *   - 配置 bean（[[=Config{"prefix"}]]）：由 config::bind 按前缀绑定
     *     （不调构造器；需要 app 提供配置）
     *   - 普通 bean：重载决议构造，参数为依赖 bean 左值（引用直连）
     */
    template <class T>
    void construct_bean() {
        if constexpr (detail::is_config_bean(^^T)) {
            if (!cfg_)
                throw std::runtime_error(
                    "cboot: config bean requires config (set application::config_path)");
            constexpr auto anno = detail::config_anno<T>;
            std::get<std::unique_ptr<T>>(beans_) =
                std::make_unique<T>(config::bind<T>(*cfg_, anno.prefix.view()));
        } else {
            constexpr std::size_t D = detail::dep_count<T>();
            if constexpr (D == 0) {
                std::get<std::unique_ptr<T>>(beans_) = std::make_unique<T>();
            } else {
                [&]<std::size_t... J>(std::index_sequence<J...>) {
                    std::get<std::unique_ptr<T>>(beans_) =
                        std::make_unique<T>(arg<T, J>()...);
                }(std::make_index_sequence<D>{});
            }
        }
    }

    /** @brief 第 J 个依赖实参：内建（配置）或清单 bean → 引用 */
    template <class T, std::size_t J>
    auto& arg() {
        constexpr std::meta::info dep = detail::nth_dep<T, J>();
        if constexpr (detail::is_config_dep(dep)) {
            if (!cfg_)
                throw std::runtime_error(
                    "cboot: config not provided (set application::config_path)");
            return *cfg_;                       // const config::properties&
        } else {
            constexpr std::size_t I = detail::bean_index<dep, Beans...>();
            auto& p = std::get<I>(beans_);
            if (!p) throw std::runtime_error("cboot: dependency null (topo order bug)");
            return *p;
        }
    }

    template <std::size_t I = 0>
    void post_construct_indexed(std::size_t i) {
        if constexpr (I < sizeof...(Beans)) {
            if (i == I) post_construct_one<I>();
            else post_construct_indexed<I + 1>(i);
        }
    }

    template <std::size_t I>
    void post_construct_one() {
        using T = bean_t<I>;
        constexpr std::meta::info fn = detail::lifecycle_fn<T>(^^PostConstruct);
        if constexpr (fn != std::meta::info{}) {
            (*std::get<I>(beans_)).[:fn:]();
        }
    }

    // ── PreDestroy（逆拓扑序）───────────────────────────────────────────

    void pre_destroy_all() {
        constexpr auto order = detail::topo_order<Beans...>();
        for (std::size_t k = order.size(); k-- > 0;)
            pre_destroy_indexed(order[k]);
    }

    template <std::size_t I = sizeof...(Beans)>
    void pre_destroy_indexed(std::size_t i) {
        if constexpr (I > 0) {
            if (i == I - 1) pre_destroy_one<I - 1>();
            else pre_destroy_indexed<I - 1>(i);
        }
    }

    template <std::size_t I>
    void pre_destroy_one() {
        using T = bean_t<I>;
        constexpr std::meta::info fn = detail::lifecycle_fn<T>(^^PreDestroy);
        if constexpr (fn != std::meta::info{}) {
            if (auto& p = std::get<I>(beans_)) ( *p ).[:fn:]();
        }
    }
};

// ==================== 清单 → 容器（自动发现/清单模式的衔接）====================

// 注：类模板偏特化不能置于无花括号 export 声明中——用 export{} 块
export {
/** @brief 类型清单 → 容器类型：context_for<type_list<A, B, C>> == context<A, B, C> */
template <class L>
struct context_for_impl;
template <class... Ts>
struct context_for_impl<type_list<Ts...>> {
    using type = context<Ts...>;
};
template <class L>
using context_for = typename context_for_impl<L>::type;
}
