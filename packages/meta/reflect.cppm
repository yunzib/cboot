export module cboot.meta.reflect;

import std;    // std::meta 随 std 模块收编；-freflection 由 cboot_meta 目标 PUBLIC 注入

/**
 * @brief 反射基建 — R1 工具集 + P3394 注解工具（kwik-ui 纪律 + GCC16.2 实测定型）
 *
 * GCC16 (P2996) 实现纪律（违反即编译错）：
 *   1. nonstatic_data_members_of 返回的 vector<info> 只能在 consteval 内
 *      消费（不能外持有 constexpr 变量）——所有元查询收敛到下面的
 *      consteval 访问器（member_count / nth_member / nth_member_name）
 *   2. std::meta::info 不能作运行时函数参数——成员访问经 index_sequence
 *      展开 + [:r:] splice 完成编译期注入
 *   3. identifier_of 返回的 string_view 底层字符数组带 '\0' 终止
 *      （nth_member_name 内 consteval 断言锁死）——.data() 可安全外传
 *
 * 辅助函数不放匿名命名空间（export template 实例化会"暴露 TU-local
 * 实体"的 GCC 报错）——模块作用域普通模板即可。
 */

/** @brief P 的非静态数据成员数（consteval：vector<info> 消费纪律） */
export template <typename P>
consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(^^P, std::meta::access_context::unchecked()).size();
}

/** @brief P 的第 I 个非静态数据成员的反射 info（consteval；供下游 splice 注入） */
export template <typename P, std::size_t I>
consteval std::meta::info nth_member() {
    return std::meta::nonstatic_data_members_of(^^P, std::meta::access_context::unchecked())[I];
}

/**
 * @brief 第 I 个成员的名字（consteval；'\0' 终止编译期锁死）
 *
 * 返回的 string_view .data() 可安全传给要求 const char* 的 C 接口
 */
export template <typename P, std::size_t I>
consteval std::string_view nth_member_name() {
    std::string_view sv = std::meta::identifier_of(nth_member<P, I>());
    if (sv.data() == nullptr || sv.data()[sv.size()] != '\0') {
        throw "identifier_of must be null-terminated";
    }
    return sv;
}

/**
 * @brief R 的非静态数据成员表（consteval；vector<info> → array 适配器）
 *
 * GCC16 纪律下 vector<info> 不能作 constexpr 变量持有、不能直接作
 * template for 的 range（非常量表达式——operator new 结果）。本适配器
 * 转成 std::array 后两条路皆通：
 *   constexpr auto mems = members_array<^^P>();       // constexpr 持有
 *   template for (constexpr auto m : mems) { ... }    // P1306 展开遍历
 * 尺寸取自直调 size()（constexpr 局部），元素按下标重调取值（kwik
 * nth_member 同款手法，避免持有 vector 本体）
 */
export template <std::meta::info R>
consteval auto members_array() {
    constexpr std::size_t n =
        std::meta::nonstatic_data_members_of(R, std::meta::access_context::unchecked()).size();
    std::array<std::meta::info, n> arr{};
    for (std::size_t i = 0; i < n; ++i)
        arr[i] = std::meta::nonstatic_data_members_of(R, std::meta::access_context::unchecked())[i];
    return arr;
}

/**
 * @brief 逐成员展开：f(名字, 成员引用) 对 P 的每个非静态数据成员调用一次
 *
 * 成员引用经 [:r:] splice 注入（编译期），auto&& 按成员类型退化——
 * 调用方据此做类型分派。DI/配置绑定/JSON 序列化的共同基建。
 */
export template <typename P, typename F>
void for_each_member(P& obj, F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (f(nth_member_name<P, I>(), obj.[:nth_member<P, I>():]), ...);
    }(std::make_index_sequence<member_count<P>()>{});
}

/** @brief for_each_member 的 const 视图（比较/只读序列化路径） */
export template <typename P, typename F>
void for_each_member(const P& obj, F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (f(nth_member_name<P, I>(), obj.[:nth_member<P, I>():]), ...);
    }(std::make_index_sequence<member_count<P>()>{});
}

// ==================== 类型清单（编译期 bean 集合的载体）====================

/** @brief 类型清单：把反射得到的类型序列物化为可作模板实参的包 */
export template <class... Ts>
struct type_list {};

// 注：类模板偏特化不能置于无花括号 export 声明中——统一用 export{} 块
export {

/** @brief L 是否 type_list<...>（组件描述符字段校验用） */
template <class L>
struct is_type_list : std::false_type {};
template <class... Ts>
struct is_type_list<type_list<Ts...>> : std::true_type {};
template <class L>
inline constexpr bool is_type_list_v = is_type_list<L>::value;

/** @brief 多个 type_list 顺序拼接；concat_t<> == type_list<> */
template <class... Ls>
struct concat_lists;

template <>
struct concat_lists<> { using type = type_list<>; };

template <class... Ts>
struct concat_lists<type_list<Ts...>> { using type = type_list<Ts...>; };

template <class... As, class... Bs, class... Rest>
struct concat_lists<type_list<As...>, type_list<Bs...>, Rest...> {
    using type = typename concat_lists<type_list<As..., Bs...>, Rest...>::type;
};

template <class... Ls>
using concat_t = typename concat_lists<Ls...>::type;

/** @brief T 是否出现在 L 中 */
template <class T, class L>
struct type_list_contains : std::false_type {};
template <class T, class... Ts>
struct type_list_contains<T, type_list<Ts...>>
    : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};

/** @brief 按类型去重，保留首次出现顺序（组件闭包/bean 合并共用） */
template <class In, class Acc = type_list<>>
struct unique_impl;

template <class... Us>
struct unique_impl<type_list<>, type_list<Us...>> {
    using type = type_list<Us...>;
};

template <class T, class... Ts, class... Us>
struct unique_impl<type_list<T, Ts...>, type_list<Us...>> {
    using next = std::conditional_t<
        type_list_contains<T, type_list<Us...>>::value,
        type_list<Us...>,
        type_list<Us..., T>>;
    using type = typename unique_impl<type_list<Ts...>, next>::type;
};

template <class L>
using unique_t = typename unique_impl<L>::type;

} // export {

// ==================== info → type 桥接（GCC16 splice 模板实参限制的解）=====

/**
 * @brief 反射 info → 类型的桥接
 *
 * GCC16.2 限制：splice 不能直接作模板实参（unparenthesized 被拒、
 * 括号化 <([:t:])> 被解析为表达式 splice）——但 using 别名
 * `using P = [:t:]` 可用。经别名间接获得可作模板实参的类型：
 *   std::is_aggregate_v<type_of_t<r>>
 *   json::get_as<type_of_t<param_type>>(...)
 */
export template <std::meta::info t>
struct type_of { using type = [:t:]; };

export template <std::meta::info t>
using type_of_t = typename type_of<t>::type;

// ==================== 注解工具（P3394R4，GCC16.2 验证形态）====================
//
// R4 口径：检索必须走 annotations_of_with_type(r, ^^A)——annotations_of 返回
// 的 info 上 identifier_of(type_of(a)) 会 throw（has_identifier false），手动
// type_of 匹配路不通。注解值类型须可 constexpr 构造（字符串用字面量 char 数组
// 或 fixed_string，std::string 不可）。

/**
 * @brief 取 r 上类型为 A 的第一个注解反射（consteval）
 * @return 命中的注解 info；无注解返回 nullopt
 */
export template <typename A>
consteval std::optional<std::meta::info> find_annotation(std::meta::info r) {
    for (auto a : std::meta::annotations_of_with_type(r, ^^A)) return a;
    return std::nullopt;
}

/**
 * @brief 取 r 上类型为 A 的第一个注解值（consteval）
 * @return extract 后的注解值；无注解返回 nullopt
 */
export template <typename A>
consteval std::optional<A> annotation_of(std::meta::info r) {
    for (auto a : std::meta::annotations_of_with_type(r, ^^A))
        return std::meta::extract<A>(a);
    return std::nullopt;
}

// ==================== enum 注册表（element_type.cpp 同款手法）====================

/** @brief E 的枚举器数（consteval：vector<info> 消费纪律） */
export template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t enumerator_count() {
    return std::meta::enumerators_of(^^E).size();
}

/** @brief E 的第 I 个枚举器反射（consteval；不持有 vector<info> 本体） */
export template <typename E, std::size_t I>
    requires std::is_enum_v<E>
consteval std::meta::info nth_enumerator() {
    return std::meta::enumerators_of(^^E)[I];
}

/**
 * @brief E 的 (枚举名, 枚举值) 对表（consteval；枚举值经 splice 注入，不依赖 extract）
 *
 * GCC16 纪律：splice 操作数须为常量表达式形态——运行时循环变量不可 splice，
 * 故经 index_sequence + nth_enumerator 常量调用展开（kwik nth_member 同款）
 */
template <typename E>
    requires std::is_enum_v<E>
consteval auto enum_entries() {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<std::pair<std::string_view, E>, sizeof...(I)>{
            std::make_pair(std::meta::identifier_of(nth_enumerator<E, I>()),
                           [:nth_enumerator<E, I>():])...};
    }(std::make_index_sequence<enumerator_count<E>()>{});
}

/** @brief E 的全部枚举名（consteval；顺序 = enumerators_of 声明序） */
export template <typename E>
    requires std::is_enum_v<E>
consteval auto enum_names() {
    auto entries = enum_entries<E>();
    std::array<std::string_view, entries.size()> out{};
    for (std::size_t i = 0; i < entries.size(); ++i) out[i] = entries[i].first;
    return out;
}

/** @brief 枚举值 → 名字（运行时查表；未命中返回空串） */
export template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view enum_to_name(E v) {
    constexpr auto entries = enum_entries<E>();    // constexpr 局部 → 常量求值内完成 consteval 调用
    for (const auto& [name, value] : entries) {
        if (value == v) return name;
    }
    return {};
}

/** @brief 名字 → 枚举值（运行时查表；未命中返回 nullopt） */
export template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> enum_from_name(std::string_view name) {
    constexpr auto entries = enum_entries<E>();
    for (const auto& [n, value] : entries) {
        if (n == name) return value;
    }
    return std::nullopt;
}
