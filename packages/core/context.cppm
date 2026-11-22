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
 *     - 依赖提取（parameters_of → 剥 cv/引用；支持基类匹配与 [[=Named{...}]]）
 *     - 依赖解析（类型 + 名字 → 清单下标；多候选/缺失/重名给诊断）
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

/** @brief 事件发布结果（同步派发：成功/失败监听者计数） */
export struct event_result {
    std::size_t delivered = 0;
    std::size_t failed = 0;
};

/** @brief 监听者异常处理器：默认打印到 stderr 并继续后续监听者 */
export using event_error_handler = std::function<void(
    std::string_view bean, std::string_view method, std::string_view what)>;

/**
 * @brief 注入式事件发布器（容器提供，本身不是 bean）
 *
 *   class OrderService {
 *   public:
 *       OrderService(event_publisher& bus) : bus_(bus) {}
 *       void place(int id) { bus_.publish(OrderPlaced{id}); }
 *   private:
 *       event_publisher& bus_;
 *   };
 *
 * 事件类型 → 派发函数的映射由容器在启动时按监听者参数类型建立；
 * 未注册的事件类型返回空结果（delivered=0）。
 */
export class event_publisher {
public:
    using dispatch_fn = event_result (*)(void*, const void*);
    using dispatch_map = std::unordered_map<std::type_index, dispatch_fn>;

    event_publisher() = default;
    event_publisher(const event_publisher&) = delete;
    event_publisher& operator=(const event_publisher&) = delete;

    template <class E>
    event_result publish(const E& e) const {
        if (!map_) throw std::runtime_error("cboot: event_publisher not bound");
        const auto it = map_->find(std::type_index(typeid(E)));
        if (it == map_->end()) return {};
        return it->second(ctx_, &e);
    }

    /** @brief 设置监听者异常处理器（默认写 stderr） */
    void set_error_handler(event_error_handler h) {
        error_ = std::move(h);
    }

    /** @internal 容器/派发路径调用 */
    void report_error(std::string_view bean, std::string_view method,
                      std::string_view what) const {
        if (error_) {
            try {
                error_(bean, method, what);
                return;
            } catch (...) {
                // 处理器自身失败不能打断事件派发——退回默认 stderr
            }
        }
        std::cerr << "[cboot] event listener " << bean << "." << method
                  << " failed: " << what << '\n';
    }

    /** @internal 容器启动时绑定 */
    void bind(void* ctx, const dispatch_map* map) {
        ctx_ = ctx;
        map_ = map;
    }

private:
    void* ctx_ = nullptr;
    const dispatch_map* map_ = nullptr;
    event_error_handler error_;
};

/** @brief 健康状态快照 */
export struct health_snapshot {
    bool healthy = true;
    std::string reason;
};

/**
 * @brief 健康状态登记器（容器提供，本身不是 bean）
 *
 * 业务 bean 可注入 `health_registry&`，在依赖不可用时标记 DOWN；
 * `/health` 读取该状态返回 200/503。
 */
export class health_registry {
public:
    health_registry() = default;
    health_registry(const health_registry&) = delete;
    health_registry& operator=(const health_registry&) = delete;

    void set_healthy() {
        std::lock_guard lock(mu_);
        healthy_ = true;
        reason_.clear();
    }

    void set_unhealthy(std::string reason) {
        std::lock_guard lock(mu_);
        healthy_ = false;
        reason_ = std::move(reason);
    }

    health_snapshot snapshot() const {
        std::lock_guard lock(mu_);
        return {healthy_, reason_};
    }

private:
    mutable std::mutex mu_;
    bool healthy_ = true;
    std::string reason_;
};

/** @brief bean 元数据（/beans 用；字符串均为静态存储） */
export struct bean_info {
    std::string_view type;
    std::string_view name;
    std::string_view profile;
    bool active = false;
    std::vector<std::string_view> deps;
};

namespace detail {

/** @brief 内建依赖：config::properties（由 app 加载后注入，不算 bean） */
consteval bool is_config_dep(std::meta::info t) { return t == ^^config::properties; }

/** @brief 内建依赖：event_publisher（由容器提供，不算 bean） */
consteval bool is_event_publisher_dep(std::meta::info t) {
    return t == ^^event_publisher;
}

/** @brief 内建依赖：health_registry（由容器提供，不算 bean） */
consteval bool is_health_registry_dep(std::meta::info t) {
    return t == ^^health_registry;
}

consteval bool is_builtin_dep(std::meta::info t) {
    return is_config_dep(t) || is_event_publisher_dep(t)
        || is_health_registry_dep(t);
}

/** @brief consteval 十进制转换（std::to_string 在 libstdc++ 非 constexpr） */
consteval std::string dec(std::size_t v) {
    if (v == 0) return "0";
    char buf[32];
    std::size_t i = sizeof buf;
    while (v != 0) {
        buf[--i] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    return std::string(buf + i, sizeof buf - i);
}

/** @brief 配置 bean 判据：类型带 [[=Config{...}]] 标注 */
consteval bool is_config_bean(std::meta::info T) {
    return std::meta::annotations_of_with_type(T, ^^Config).size() > 0;
}

/** @brief 以模板参数传类型，避免 ^^局部别名 反映到别名实体 */
template <class T>
consteval bool is_config_bean_type() { return is_config_bean(^^T); }

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
        + "' has " + dec(total)
        + " constructors: annotate the injected one with [[=Inject{}]]");
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

/** @brief bean 的限定名：Bean.name；配置 bean 取 Config.prefix；未命名 = 空 */
template <class T>
inline constexpr bean_name bean_name_of = []() consteval {
    for (auto a : std::meta::annotations_of_with_type(^^T, ^^Bean))
        return std::meta::extract<Bean>(a).name;
    for (auto a : std::meta::annotations_of_with_type(^^T, ^^Config))
        return std::meta::extract<Config>(a).prefix;
    return bean_name{};
}();

/** @brief bean 的 Profile 名（无注解 = 空，始终激活） */
template <class T>
inline constexpr bean_name profile_of = []() consteval {
    for (auto a : std::meta::annotations_of_with_type(^^T, ^^Profile))
        return std::meta::extract<Profile>(a).name;
    return bean_name{};
}();

/** @brief 依赖 D 能否绑定到候选 B：同类型，或 B 可转换为 D（public 基类） */
template <class D, class B>
consteval bool bean_matches() {
    if constexpr (std::is_same_v<D, B>) return true;
    else if constexpr (std::is_class_v<D> && std::is_class_v<B>)
        return std::is_convertible_v<B*, D*>;
    else return false;
}

template <class D, class... Bs>
consteval auto match_mask() {
    std::array<bool, sizeof...(Bs)> m{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((m[I] = bean_matches<D, Bs...[I]>()), ...);
    }(std::make_index_sequence<sizeof...(Bs)>{});
    return m;
}

template <class... Bs>
consteval auto bean_names() {
    std::array<std::string_view, sizeof...(Bs)> a{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((a[I] = bean_name_of<Bs...[I]>.view()), ...);
    }(std::make_index_sequence<sizeof...(Bs)>{});
    return a;
}

template <class T>
consteval std::string_view type_name_of() {
    return std::meta::identifier_of(^^T);
}

template <class... Bs>
consteval auto type_names() {
    std::array<std::string_view, sizeof...(Bs)> a{};
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((a[I] = type_name_of<Bs...[I]>()), ...);
    }(std::make_index_sequence<sizeof...(Bs)>{});
    return a;
}

// ── 事件监听方法枚举（编译期收集，publish 时按事件类型精确匹配）──────────

template <class T>
consteval bool is_listener(std::meta::info m) {
    return std::meta::is_function(m) && std::meta::is_class_member(m)
        && std::meta::annotations_of_with_type(m, ^^EventListener).size() > 0;
}

template <class T>
consteval std::size_t listener_count() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
        if (is_listener<T>(m)) ++n;
    return n;
}

template <class T, std::size_t I>
consteval std::meta::info nth_listener() {
    std::size_t seen = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (is_listener<T>(m)) {
            if (seen == I) return m;
            ++seen;
        }
    }
    throw "cboot: listener index out of range";
}

template <class T, std::size_t I>
consteval std::meta::info listener_param() {
    return strip_dep(std::meta::type_of(
        std::meta::parameters_of(nth_listener<T, I>())[0]));
}

template <class T, std::size_t I>
consteval std::string_view listener_name() {
    return std::meta::identifier_of(nth_listener<T, I>());
}

/** @brief 监听方法契约：public、非静态、一个参数、按值或 const 引用 */
template <class T>
consteval void validate_listener() {
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (!is_listener<T>(m)) continue;
        const std::string name = std::string(std::meta::identifier_of(^^T))
            + "." + std::string(std::meta::identifier_of(m));
        if (std::meta::is_static_member(m))
            throw std::define_static_string(
                "cboot: listener '" + name + "' must be a non-static member function");
        if (!std::meta::is_public(m))
            throw std::define_static_string(
                "cboot: listener '" + name + "' must be public");
        auto ps = std::meta::parameters_of(m);
        if (ps.size() != 1)
            throw std::define_static_string(
                "cboot: listener '" + name
                + "' must take exactly one event parameter");
        const auto raw = std::meta::type_of(ps[0]);
        if (std::meta::is_rvalue_reference_type(raw)
            || (std::meta::is_lvalue_reference_type(raw)
                && !std::meta::is_const_type(
                    std::meta::remove_reference(raw))))
            throw std::define_static_string(
                "cboot: listener '" + name
                + "' must take the event by value or const reference");
    }
}

template <class... Bs>
consteval void validate_listeners() {
    (validate_listener<Bs>(), ...);
}

// ── 定时任务方法枚举（编译期收集，容器启动后单线程调度）────────────────

template <class T>
consteval bool is_scheduled(std::meta::info m) {
    return std::meta::is_function(m) && std::meta::is_class_member(m)
        && std::meta::annotations_of_with_type(m, ^^Scheduled).size() > 0;
}

template <class T>
consteval std::size_t scheduled_count() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
        if (is_scheduled<T>(m)) ++n;
    return n;
}

template <class T, std::size_t I>
consteval std::meta::info nth_scheduled() {
    std::size_t seen = 0;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (is_scheduled<T>(m)) {
            if (seen == I) return m;
            ++seen;
        }
    }
    throw "cboot: scheduled index out of range";
}

consteval Scheduled scheduled_anno_of(std::meta::info m) {
    for (auto a : std::meta::annotations_of_with_type(m, ^^Scheduled))
        return std::meta::extract<Scheduled>(a);
    return Scheduled{};
}

template <class T, std::size_t I>
consteval Scheduled scheduled_anno() {
    return scheduled_anno_of(nth_scheduled<T, I>());
}

template <class T, std::size_t I>
consteval std::string_view scheduled_name() {
    return std::meta::identifier_of(nth_scheduled<T, I>());
}

/** @brief 定时方法契约：public、非静态、无参数、delay > 0 */
template <class T>
consteval void validate_scheduled() {
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
        if (!is_scheduled<T>(m)) continue;
        const std::string name = std::string(std::meta::identifier_of(^^T))
            + "." + std::string(std::meta::identifier_of(m));
        if (std::meta::is_static_member(m))
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' must be a non-static member function");
        if (!std::meta::is_public(m))
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' must be public");
        if (!std::meta::parameters_of(m).empty())
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' must take no parameters");
        if (scheduled_anno_of(m).fixed_delay_ms <= 0)
            throw std::define_static_string(
                "cboot: scheduled '" + name + "' requires fixed_delay_ms > 0");
    }
}

template <class... Bs>
consteval void validate_scheduled_all() {
    (validate_scheduled<Bs>(), ...);
}


template <class T, std::size_t J>
inline constexpr bean_name dep_qualifier = []() consteval {
    constexpr auto c = select_ctor<T>();
    if constexpr (!c.has_value()) return bean_name{};
    else {
        for (auto a : std::meta::annotations_of_with_type(
                 std::meta::parameters_of(*c)[J], ^^Named))
            return std::meta::extract<Named>(a).name;
        return bean_name{};
    }
}();

/** @brief 第 J 个构造器参数的声明类型（保留 cv/引用） */
template <class T, std::size_t J>
consteval std::meta::info nth_param() {
    constexpr auto c = select_ctor<T>();
    return std::meta::type_of(std::meta::parameters_of(*c)[J]);
}

template <class T, std::size_t J>
using param_t = type_of_t<nth_param<T, J>()>;

/** @brief 第 J 个构造器参数的 [[=Value{"key"}]]（无注解 = 空） */
template <class T, std::size_t J>
inline constexpr bean_name dep_value = []() consteval {
    constexpr auto c = select_ctor<T>();
    if constexpr (!c.has_value()) return bean_name{};
    else {
        for (auto a : std::meta::annotations_of_with_type(
                 std::meta::parameters_of(*c)[J], ^^Value))
            return std::meta::extract<Value>(a).key;
        return bean_name{};
    }
}();

/** @brief 第 J 个参数是否带 [[=Value{}]]（区分"无注解"与"空 key"） */
template <class T, std::size_t J>
inline constexpr bool dep_has_value = []() consteval {
    constexpr auto c = select_ctor<T>();
    if constexpr (!c.has_value()) return false;
    else return std::meta::annotations_of_with_type(
        std::meta::parameters_of(*c)[J], ^^Value).size() > 0;
}();

/** @brief [[=Value{}]] 支持的标量类型 */
template <class P>
consteval bool is_value_type() {
    return std::is_same_v<P, bool> || std::is_integral_v<P>
        || std::is_floating_point_v<P> || std::is_same_v<P, std::string>
        || std::is_enum_v<P>;
}

/** @brief 配置字符串 → 标量；失败抛带 key/owner 的运行时错误 */
template <class P>
P parse_value(const std::string& raw, std::string_view key,
              std::string_view owner) {
    static_assert(is_value_type<P>(),
                  "cboot: [[=Value{}]] supports bool/integral/floating/string/enum");
    if constexpr (std::is_same_v<P, bool>) {
        if (raw == "true" || raw == "1") return true;
        if (raw == "false" || raw == "0") return false;
    } else if constexpr (std::is_integral_v<P>) {
        P v{};
        const auto [p, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (ec == std::errc{} && p == raw.data() + raw.size()) return v;
    } else if constexpr (std::is_floating_point_v<P>) {
        P v{};
        const auto [p, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (ec == std::errc{} && p == raw.data() + raw.size()) return v;
    } else if constexpr (std::is_same_v<P, std::string>) {
        return raw;
    } else if constexpr (std::is_enum_v<P>) {
        if (auto n = enum_from_name<P>(raw)) return *n;
    }
    throw std::runtime_error(
        "cboot: invalid config value for '" + std::string(key)
        + "' required by '" + std::string(owner) + "': '" + raw + "'");
}

/**
 * @brief 依赖解析：类型（含派生）+ [[=Named{...}]] → 清单下标
 *
 * 规则：
 *   1. 带名字：候选类型匹配且 bean 名相同，必须恰好一个；
 *   2. 不带名字：候选唯一 → 用它；多候选时只接受唯一默认实例（名为空）；
 *   3. 其余情况给出候选列表/缺失诊断。
 */
template <class T, std::size_t J, class... Bs>
consteval std::size_t resolve_dep_index() {
    constexpr std::meta::info d = nth_dep<T, J>();
    constexpr auto mask = match_mask<type_of_t<d>, Bs...>();
    constexpr auto names = bean_names<Bs...>();
    constexpr auto tnames = type_names<Bs...>();
    constexpr std::string_view want = dep_qualifier<T, J>.view();
    constexpr bool named = !want.empty();

    const std::string self(type_name_of<T>());
    const std::string dep(std::meta::has_identifier(d)
        ? std::string(std::meta::identifier_of(d)) : std::string("?"));

    std::size_t hit[sizeof...(Bs)];
    std::size_t n = 0;
    for (std::size_t i = 0; i < sizeof...(Bs); ++i) {
        if (!mask[i]) continue;
        if (named && names[i] != want) continue;
        hit[n++] = i;
    }

    if (named) {
        if (n == 0)
            throw std::define_static_string(
                "cboot: '" + self + "' requires bean named '"
                + std::string(want) + "' of type '" + dep
                + "' (no match in bean list)");
        if (n > 1)
            throw std::define_static_string(
                "cboot: multiple beans named '" + std::string(want)
                + "' for '" + dep + "' required by '" + self + "'");
        return hit[0];
    }

    if (n == 0)
        throw std::define_static_string(
            "cboot: '" + self + "' requires '" + dep
            + "' which is not in the bean list (annotate it [[=Bean{}]] "
              "or add it to run<...>)");
    if (n == 1) return hit[0];

    std::size_t def = sizeof...(Bs);
    std::size_t defs = 0;
    for (std::size_t k = 0; k < n; ++k)
        if (names[hit[k]].empty()) { def = hit[k]; ++defs; }
    if (defs == 1) return def;

    std::string msg = "cboot: '" + self + "' has " + dec(n)
        + " candidates for '" + dep + "': annotate the parameter with "
          "[[=Named{\"...\"}]] (candidates: ";
    for (std::size_t k = 0; k < n; ++k) {
        if (k) msg += ", ";
        msg += names[hit[k]].empty() ? std::string(tnames[hit[k]])
                                     : std::string(names[hit[k]]);
    }
    msg += ")";
    throw std::define_static_string(msg);
}

/** @brief get<T>() 的候选解析：唯一，或唯一默认实例 */
template <class T, class... Bs>
consteval std::size_t select_default_index() {
    constexpr auto mask = match_mask<T, Bs...>();
    constexpr auto names = bean_names<Bs...>();
    std::size_t hit[sizeof...(Bs)];
    std::size_t n = 0;
    for (std::size_t i = 0; i < sizeof...(Bs); ++i)
        if (mask[i]) hit[n++] = i;

    const std::string type(type_name_of<T>());
    if (n == 0)
        throw std::define_static_string(
            "cboot: no bean of type '" + type + "' in bean list");
    if (n == 1) return hit[0];

    std::size_t def = sizeof...(Bs);
    std::size_t defs = 0;
    for (std::size_t k = 0; k < n; ++k)
        if (names[hit[k]].empty()) { def = hit[k]; ++defs; }
    if (defs == 1) return def;

    throw std::define_static_string(
        "cboot: multiple beans match '" + type
        + "': use get<T>(name) or [[=Named{...}]]");
}

/** @brief 构造器参数 K 解析到下标 J 时命中 */
template <class A, std::size_t K, std::size_t J, class... Bs>
consteval bool dep_hits() {
    constexpr std::size_t D = dep_count<A>();
    if constexpr (K >= D) return false;
    else {
        constexpr std::meta::info d = nth_dep<A, K>();
        if constexpr (dep_has_value<A, K>)
            return dep_hits<A, K + 1, J, Bs...>();
        if constexpr (is_builtin_dep(d))
            return dep_hits<A, K + 1, J, Bs...>();
        else if (resolve_dep_index<A, K, Bs...>() == J)
            return true;
        else
            return dep_hits<A, K + 1, J, Bs...>();
    }
}

template <std::size_t I, std::size_t J, class... Bs>
consteval bool depends_on_pair() {
    return dep_hits<Bs...[I], 0, J, Bs...>();
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
            ((deps[I * N + J] = depends_on_pair<I, J, Bs...>()), ...);
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

/** @brief 依赖满足校验（解析失败即编译期诊断） */
template <class T, std::size_t J, class... Bs>
consteval void check_dep() {
    static_assert(!dep_has_value<T, J> || !dep_value<T, J>.view().empty(),
                  "cboot: [[=Value{}]] requires a non-empty config key");
    if constexpr (dep_has_value<T, J>) {
        using Raw = param_t<T, J>;
        using P = std::remove_cvref_t<Raw>;
        static_assert(dep_qualifier<T, J>.view().empty(),
                      "cboot: parameter cannot use both [[=Value{}]] and [[=Named{}]]");
        static_assert(!std::is_reference_v<Raw>
                      || std::is_const_v<std::remove_reference_t<Raw>>,
                      "cboot: [[=Value{}]] parameter must be a value or const reference");
        static_assert(is_value_type<P>(),
                      "cboot: [[=Value{}]] supports bool/integral/floating/string/enum");
        return;
    }
    constexpr std::meta::info d = nth_dep<T, J>();
    if constexpr (!is_builtin_dep(d)) (void)resolve_dep_index<T, J, Bs...>();
}

template <class T, class... Bs>
consteval void validate_deps() {
    constexpr std::size_t D = dep_count<T>();
    [&]<std::size_t... J>(std::index_sequence<J...>) {
        (check_dep<T, J, Bs...>(), ...);
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
    explicit context(const config::properties* cfg = nullptr) : cfg_(cfg) {
        const auto t0 = std::chrono::steady_clock::now();
        constexpr bool listeners_ok =
            (detail::validate_listeners<Beans...>(), true);
        constexpr bool scheduled_ok =
            (detail::validate_scheduled_all<Beans...>(), true);
        (void)listeners_ok;
        (void)scheduled_ok;
        register_events();
        publisher_.bind(this, &event_map_);
        init_profiles();
        build_all();
        register_scheduled_tasks();
        start_scheduler();
        startup_ms_ = static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }
    ~context() {
        stop_scheduler();
        pre_destroy_all();
    }
    context(const context&) = delete;
    context& operator=(const context&) = delete;

    /** @brief 清单下标 I 的 bean 在当前 profile 下是否激活 */
    template <std::size_t I>
    bool is_active() const { return active_[I]; }

    /** @brief 当前健康状态快照（/health 用） */
    health_snapshot health() const { return health_.snapshot(); }

    /** @brief 健康登记器（业务 bean 注入 health_registry& 同款；测试可直接取） */
    health_registry& health_check() { return health_; }

    /** @brief 激活 bean 数量 */
    std::size_t active_bean_count() const {
        std::size_t n = 0;
        for (bool a : active_)
            if (a) ++n;
        return n;
    }

    /** @brief 容器构造耗时（毫秒，含 bean 构造与 PostConstruct） */
    std::size_t startup_ms() const { return startup_ms_; }

    /** @brief 当前激活的 profile 中是否包含 name */
    bool has_profile(std::string_view name) const {
        for (const auto& p : active_profiles_)
            if (std::string_view(p) == name) return true;
        return false;
    }

    /** @brief bean 清单与依赖（/beans 用；字符串指向静态存储） */
    std::vector<bean_info> beans_info() const {
        std::vector<bean_info> out;
        out.reserve(sizeof...(Beans));
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (out.push_back(make_bean_info<I>()), ...);
        }(std::make_index_sequence<sizeof...(Beans)>{});
        return out;
    }

    /** @brief 按类型取 bean（唯一候选，或候选中的唯一默认实例） */
    template <class T>
    T& get() {
        constexpr std::size_t I = detail::select_default_index<T, Beans...>();
        if (!active_[I]) throw inactive_error<bean_t<I>>();
        auto& p = std::get<I>(beans_);
        if (!p) throw std::runtime_error("cboot: bean not constructed");
        return static_cast<T&>(*p);
    }

    /** @brief 按类型 + bean 名取（与 [[=Named{...}]] 同款名字匹配） */
    template <class T>
    T& get(std::string_view name) {
        if (T* p = find_named<T>(name)) return *p;
        throw std::runtime_error(
            "cboot: no active bean named '" + std::string(name) + "'");
    }

    /** @brief 按清单下标取（挂载全部 bean 用，绕过类型歧义） */
    template <std::size_t I>
    auto& get_at() {
        if (!active_[I]) throw inactive_error<bean_t<I>>();
        auto& p = std::get<I>(beans_);
        if (!p) throw std::runtime_error("cboot: bean not constructed");
        return *p;
    }

    /**
     * @brief 发布事件：同步调用所有参数类型为 E 的监听方法
     *
     * 顺序：bean 清单顺序 → 方法声明顺序；未激活 bean 跳过。
     * 监听者异常默认记录到 stderr 并继续，其他监听者不受影响。
     */
    template <class E>
    event_result publish(const E& event) {
        return publish(event, [](std::string_view, std::string_view,
                                 auto&& fn) { fn(); });
    }

    /**
     * @brief 发布事件并用 wrap 包裹每个监听者调用
     *
     *   wrap(bean, method, fn) 可调用 fn()，也可接 aspect_chain：
     *     chain.run(call_info{b, m}, fn);
     */
    template <class E, class Wrap>
    event_result publish(const E& event, Wrap&& wrap) {
        event_result r;
        publish_all(event, wrap, r,
                    std::make_index_sequence<sizeof...(Beans)>{});
        return r;
    }

    /** @brief 监听者异常处理（默认打印到 stderr 并继续） */
    void set_event_error_handler(event_error_handler h) {
        publisher_.set_error_handler(std::move(h));
    }

private:
    template <std::size_t I>
    using bean_t = typename std::tuple_element_t<I,
        std::tuple<std::unique_ptr<Beans>...>>::element_type;

    /** @brief 按名字查找绑定到 T 的 bean（T 可为基类） */
    template <class T, std::size_t I = 0>
    T* find_named(std::string_view name) {
        if constexpr (I < sizeof...(Beans)) {
            using B = bean_t<I>;
            if constexpr (detail::bean_matches<T, B>()) {
                if (active_[I] && detail::bean_name_of<B>.view() == name)
                    return static_cast<T*>(std::get<I>(beans_).get());
            }
            return find_named<T, I + 1>(name);
        } else {
            return nullptr;
        }
    }

    // ── 事件派发（编译期按事件类型匹配，运行期同步调用）──────────────

    /** @brief 为事件类型 E 注册类型擦除派发入口（注入式 publisher 使用） */
    template <class E>
    void register_event() {
        event_map_[std::type_index(typeid(E))] =
            [](void* self, const void* e) -> event_result {
                return static_cast<context*>(self)->publish(
                    *static_cast<const E*>(e));
            };
    }

    template <class T, std::size_t J>
    void register_listener_event() {
        using E = type_of_t<detail::listener_param<T, J>()>;
        register_event<E>();
    }

    template <std::size_t I = 0>
    void register_events() {
        if constexpr (I < sizeof...(Beans)) {
            using T = bean_t<I>;
            constexpr std::size_t N = detail::listener_count<T>();
            [&]<std::size_t... J>(std::index_sequence<J...>) {
                (register_listener_event<T, J>(), ...);
            }(std::make_index_sequence<N>{});
            register_events<I + 1>();
        }
    }

    template <class E, class W, std::size_t... I>
    void publish_all(const E& e, W& wrap, event_result& r,
                     std::index_sequence<I...>) {
        (publish_bean<I>(e, wrap, r), ...);
    }

    template <std::size_t I, class E, class W>
    void publish_bean(const E& e, W& wrap, event_result& r) {
        if (!active_[I]) return;
        constexpr std::size_t N = detail::listener_count<bean_t<I>>();
        [&]<std::size_t... J>(std::index_sequence<J...>) {
            (publish_listener<I, J>(e, wrap, r), ...);
        }(std::make_index_sequence<N>{});
    }

    template <std::size_t I, std::size_t J, class E, class W>
    void publish_listener(const E& e, W& wrap, event_result& r) {
        constexpr std::meta::info pt =
            detail::listener_param<bean_t<I>, J>();
        if constexpr (pt == ^^E) {
            auto& p = std::get<I>(beans_);
            if (!p) return;
            constexpr std::string_view bean =
                detail::type_name_of<bean_t<I>>();
            constexpr std::string_view method =
                detail::listener_name<bean_t<I>, J>();
            constexpr std::meta::info fn =
                detail::nth_listener<bean_t<I>, J>();
            try {
                wrap(bean, method, [&] { (*p).[:fn:](e); });
                ++r.delivered;
            } catch (const std::exception& ex) {
                ++r.failed;
                report_event_error(bean, method, ex.what());
            } catch (...) {
                ++r.failed;
                report_event_error(bean, method, "unknown exception");
            }
        }
    }

    void report_event_error(std::string_view bean, std::string_view method,
                            std::string_view what) const {
        publisher_.report_error(bean, method, what);
    }

    // ── 定时任务（单调度线程，固定延迟，任务异常隔离）────────────────────

    struct scheduled_task {
        std::function<void()> fn;
        std::string name;
        std::chrono::milliseconds delay{};
        std::chrono::steady_clock::time_point next{};
    };

    template <std::size_t I, std::size_t J>
    void run_scheduled() {
        auto& p = std::get<I>(beans_);
        if (!p) return;
        (*p).[:detail::nth_scheduled<bean_t<I>, J>():]();
    }

    template <std::size_t I, std::size_t J>
    void register_scheduled_task() {
        constexpr auto anno = detail::scheduled_anno<bean_t<I>, J>();
        auto& p = std::get<I>(beans_);
        if (!p) return;
        scheduled_task t;
        t.fn = [this] { run_scheduled<I, J>(); };
        t.name = std::string(detail::type_name_of<bean_t<I>>()) + "."
               + std::string(detail::scheduled_name<bean_t<I>, J>());
        t.delay = std::chrono::milliseconds(anno.fixed_delay_ms);
        t.next = std::chrono::steady_clock::now() + t.delay;
        scheduled_tasks_.push_back(std::move(t));
    }

    template <std::size_t I = 0>
    void register_scheduled_tasks() {
        if constexpr (I < sizeof...(Beans)) {
            if (active_[I]) {
                constexpr std::size_t N =
                    detail::scheduled_count<bean_t<I>>();
                [&]<std::size_t... J>(std::index_sequence<J...>) {
                    (register_scheduled_task<I, J>(), ...);
                }(std::make_index_sequence<N>{});
            }
            register_scheduled_tasks<I + 1>();
        }
    }

    static void report_scheduled_error(std::string_view name,
                                       std::string_view what) {
        std::cerr << "[cboot] scheduled task " << name
                  << " failed: " << what << '\n';
    }

    void scheduler_loop(std::stop_token st) {
        std::unique_lock lock(sched_mu_);
        while (!st.stop_requested()) {
            if (scheduled_tasks_.empty()) return;
            const auto next = std::min_element(
                scheduled_tasks_.begin(), scheduled_tasks_.end(),
                [](const auto& a, const auto& b) { return a.next < b.next; });
            if (sched_cv_.wait_until(lock, st, next->next,
                    [&] { return st.stop_requested(); }))
                break;
            if (st.stop_requested()) break;

            struct due_task {
                std::function<void()> fn;
                std::string name;
                std::size_t index;
            };
            const auto now = std::chrono::steady_clock::now();
            std::vector<due_task> due;
            for (std::size_t i = 0; i < scheduled_tasks_.size(); ++i)
                if (scheduled_tasks_[i].next <= now)
                    due.push_back({scheduled_tasks_[i].fn,
                                   scheduled_tasks_[i].name, i});

            lock.unlock();
            for (auto& d : due) {
                try {
                    d.fn();
                } catch (const std::exception& ex) {
                    report_scheduled_error(d.name, ex.what());
                } catch (...) {
                    report_scheduled_error(d.name, "unknown exception");
                }
            }
            const auto after = std::chrono::steady_clock::now();
            lock.lock();
            for (auto& d : due)
                if (d.index < scheduled_tasks_.size())
                    scheduled_tasks_[d.index].next =
                        after + scheduled_tasks_[d.index].delay;
        }
    }

    void start_scheduler() {
        if (scheduled_tasks_.empty()) return;
        scheduler_ = std::jthread(
            [this](std::stop_token st) { scheduler_loop(st); });
    }

    void stop_scheduler() {
        scheduler_.request_stop();
        sched_cv_.notify_all();
        if (scheduler_.joinable()) scheduler_.join();
    }

    template <std::size_t I>
    bean_info make_bean_info() const {
        bean_info info;
        info.type = detail::type_name_of<bean_t<I>>();
        info.name = detail::bean_name_of<bean_t<I>>.view();
        info.profile = detail::profile_of<bean_t<I>>.view();
        info.active = active_[I];
        constexpr std::size_t D = detail::dep_count<bean_t<I>>();
        [&]<std::size_t... J>(std::index_sequence<J...>) {
            (append_bean_dep<I, J>(info), ...);
        }(std::make_index_sequence<D>{});
        return info;
    }

    template <std::size_t I, std::size_t J>
    void append_bean_dep(bean_info& info) const {
        constexpr std::meta::info dep = detail::nth_dep<bean_t<I>, J>();
        if constexpr (!detail::is_builtin_dep(dep)
                      && !detail::dep_has_value<bean_t<I>, J>) {
            info.deps.push_back(std::meta::has_identifier(dep)
                ? std::meta::identifier_of(dep) : std::string_view{"?"});
        }
    }

    std::tuple<std::unique_ptr<Beans>...> beans_{};
    const config::properties* cfg_ = nullptr;
    std::array<bool, sizeof...(Beans)> active_{};
    std::vector<std::string> active_profiles_;
    event_publisher::dispatch_map event_map_;
    event_publisher publisher_;
    health_registry health_;
    std::size_t startup_ms_ = 0;
    std::vector<scheduled_task> scheduled_tasks_;
    std::mutex sched_mu_;
    std::condition_variable_any sched_cv_;
    std::jthread scheduler_;

    // ── profile 过滤（配置 cboot.profiles.active，逗号分隔）────────────────

    static std::string_view trim_profile(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix(1);
        return s;
    }

    void init_profiles() {
        if (cfg_) {
            if (auto v = cfg_->get_expanded("cboot.profiles.active")) {
                std::string_view s(*v);
                while (!s.empty()) {
                    const std::size_t comma = s.find(',');
                    std::string_view part = comma == std::string_view::npos
                        ? s : s.substr(0, comma);
                    part = trim_profile(part);
                    if (!part.empty()) active_profiles_.emplace_back(part);
                    if (comma == std::string_view::npos) break;
                    s.remove_prefix(comma + 1);
                }
            }
        }
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((active_[I] = profile_active<bean_t<I>>()), ...);
        }(std::make_index_sequence<sizeof...(Beans)>{});
    }

    template <class T>
    bool profile_active() const {
        constexpr std::string_view p = detail::profile_of<T>.view();
        if (p.empty()) return true;
        for (const auto& a : active_profiles_)
            if (std::string_view(a) == p) return true;
        return false;
    }

    std::string profiles_text() const {
        if (active_profiles_.empty()) return "<none>";
        std::string out;
        for (std::size_t i = 0; i < active_profiles_.size(); ++i) {
            if (i) out += ',';
            out += active_profiles_[i];
        }
        return out;
    }

    template <class T>
    std::runtime_error inactive_error() const {
        return std::runtime_error(
            "cboot: bean '" + std::string(detail::type_name_of<T>())
            + "' is inactive under profile (active: " + profiles_text() + ")");
    }

    template <class Owner, class Dep>
    std::runtime_error inactive_dep_error() const {
        return std::runtime_error(
            "cboot: bean '" + std::string(detail::type_name_of<Owner>())
            + "' requires '" + std::string(detail::type_name_of<Dep>())
            + "' which is inactive under profile (active: "
            + profiles_text() + ")");
    }

    // ── 构造 + PostConstruct（拓扑序逐 bean）────────────────────────────

    void build_all() {
        constexpr auto order = detail::topo_order<Beans...>();
        for (std::size_t k = 0; k < order.size(); ++k) {
            if (!active_[order[k]]) continue;
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
        construct_bean<I>();
    }

    /**
     * 构造分两路：
     *   - 配置 bean（[[=Config{"prefix"}]]）：由 config::bind 按前缀绑定
     *     （不调构造器；需要 app 提供配置）
     *   - 普通 bean：重载决议构造，参数为依赖 bean 左值（引用直连）
     */
    template <std::size_t I>
    void construct_bean() {
        using T = bean_t<I>;
        if constexpr (detail::is_config_bean_type<T>()) {
            if (!cfg_)
                throw std::runtime_error(
                    "cboot: config bean requires config (set application::config_path)");
            constexpr auto anno = detail::config_anno<T>;
            std::get<I>(beans_) =
                std::make_unique<T>(config::bind<T>(*cfg_, anno.prefix.view()));
        } else {
            constexpr std::size_t D = detail::dep_count<T>();
            if constexpr (D == 0) {
                std::get<I>(beans_) = std::make_unique<T>();
            } else {
                [&]<std::size_t... J>(std::index_sequence<J...>) {
                    std::get<I>(beans_) =
                        std::make_unique<T>(arg<T, J>()...);
                }(std::make_index_sequence<D>{});
            }
        }
    }

    /** @brief 读取并转换 [[=Value{"key"}]]；缺配置/缺键/值非法 → 明确报错 */
    template <class P>
    P read_value(std::string_view key, std::string_view owner) const {
        if (!cfg_)
            throw std::runtime_error(
                "cboot: config not provided (required by '" + std::string(owner)
                + "' for key '" + std::string(key) + "')");
        auto raw = cfg_->get_expanded(key);
        if (!raw)
            throw std::runtime_error(
                "cboot: missing config key '" + std::string(key)
                + "' required by '" + std::string(owner) + "'");
        return detail::parse_value<P>(*raw, key, owner);
    }

    /** @brief 第 J 个构造器实参：[[=Value{}]] / 内建配置 / 清单 bean */
    template <class T, std::size_t J>
    decltype(auto) arg() {
        if constexpr (detail::dep_has_value<T, J>) {
            using P = std::remove_cvref_t<detail::param_t<T, J>>;
            return read_value<P>(detail::dep_value<T, J>.view(),
                                 detail::type_name_of<T>());
        } else {
            constexpr std::meta::info dep = detail::nth_dep<T, J>();
            if constexpr (detail::is_config_dep(dep)) {
                if (!cfg_)
                    throw std::runtime_error(
                        "cboot: config not provided (set application::config_path)");
                return *cfg_;                       // const config::properties&
            } else if constexpr (detail::is_event_publisher_dep(dep)) {
                return (publisher_);                // event_publisher&
            } else if constexpr (detail::is_health_registry_dep(dep)) {
                return (health_);                   // health_registry&
            } else {
                constexpr std::size_t I =
                    detail::resolve_dep_index<T, J, Beans...>();
                if (!active_[I])
                    throw inactive_dep_error<T, bean_t<I>>();
                auto& p = std::get<I>(beans_);
                if (!p) throw std::runtime_error("cboot: dependency null (topo order bug)");
                return *p;
            }
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
