export module cboot.config;

import std;
import cboot.json;
import cboot.meta.reflect;    // for_each_member / enum 注册表

/**
 * @brief 配置绑定 — properties（扁平 key-value）→ 反射结构体
 *
 * 结构（kwik-ui 接口/实现分离纪律）：
 *   接口（本文件）：properties 类 / bind 绑定模板
 *   实现（src/config/config.cpp）：JSON 展平 / ${ENV:def} 占位符展开
 *
 * 语义（对齐 Spring @ConfigurationProperties）：
 *   - 嵌套 JSON 对象展平为 "a.b.c" 键；数组展平为 "a.0" "a.1" ...
 *   - 绑定：成员名 ≡ 键尾段；缺失键保持成员默认值
 *   - optional 成员：缺键 → nullopt
 *   - vector 成员：逗号分隔单值 或 "key.0"/"key.1" 索引键
 *   - 值支持 ${ENV_VAR} / ${ENV_VAR:default} 占位符（读取时展开）
 *
 * 支持的成员类型（同 cboot.json，超出编译错 fail-loud）：
 *   bool / 整数 / 浮点 / string / optional<U> / vector<U> / 枚举 /
 *   聚合体（递归子前缀绑定）
 */

// ── 内部工具（模块作用域，不导出）──────────────────────────────────────
namespace config::detail {

template <template <class...> class TT, class T>
struct is_specialization : std::false_type {};
template <template <class...> class TT, class... A>
struct is_specialization<TT, TT<A...>> : std::true_type {};
template <template <class...> class TT, class T>
inline constexpr bool is_specialization_v = is_specialization<TT, T>::value;

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

inline std::string join_key(std::string_view prefix, std::string_view name) {
    if (prefix.empty()) return std::string(name);
    std::string out(prefix);
    out.push_back('.');
    out.append(name);
    return out;
}

template <class T>
void convert(std::string_view raw, T& out, std::string_view key) {
    constexpr auto bad = [](std::string_view key) {
        throw std::runtime_error("cboot.config: bad value for key '" + std::string(key) + "'");
    };
    if constexpr (std::is_same_v<T, bool>) {
        out = raw == "true" || raw == "1";
    } else if constexpr (std::is_integral_v<T>) {
        T v{};
        auto [p, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (ec != std::errc{} || p != raw.data() + raw.size()) bad(key);
        out = v;
    } else if constexpr (std::is_floating_point_v<T>) {
        T v{};
        auto [p, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), v);
        if (ec != std::errc{} || p != raw.data() + raw.size()) bad(key);
        out = v;
    } else if constexpr (std::is_same_v<T, std::string>) {
        out.assign(raw);
    } else if constexpr (std::is_enum_v<T>) {
        auto n = enum_from_name<T>(raw);
        if (!n) bad(key);
        out = *n;
    } else if constexpr (is_specialization_v<std::vector, T>) {
        // 逗号分隔形式；索引键形式在 bind_to 处理
        out.clear();
        std::size_t start = 0;
        while (start <= raw.size()) {
            std::size_t end = raw.find(',', start);
            if (end == std::string_view::npos) end = raw.size();
            typename T::value_type elem{};
            convert(trim(raw.substr(start, end - start)), elem, key);
            out.push_back(std::move(elem));
            if (end == raw.size()) break;
            start = end + 1;
        }
    } else {
        static_assert(sizeof(T) == 0, "cboot.config: unsupported member type");
    }
}

} // namespace config::detail

export namespace config {

/** @brief 扁平属性源（保序；值读取时展开 ${ENV:def} 占位符） */
class properties {
public:
    /** @brief 嵌套 JSON → 扁平键（实现在 src/config/config.cpp） */
    static properties from_json(std::string_view src);

    void set(std::string key, std::string value) {
        for (auto& [k, v] : kv_)
            if (k == key) { v = std::move(value); return; }
        kv_.emplace_back(std::move(key), std::move(value));
    }

    /** @brief 覆盖合并（higher 同名键胜出——env 覆盖文件等场景） */
    void overlay(const properties& higher) {
        for (const auto& [k, v] : higher.kv_) set(k, v);
    }

    /** @brief 取值并展开占位符（未命中返回 nullopt） */
    std::optional<std::string> get_expanded(std::string_view key) const;

    /** @brief 原始取值（不展开；未命中返回 nullopt） */
    std::optional<std::string_view> get_raw(std::string_view key) const {
        for (const auto& [k, v] : kv_)
            if (k == key) return std::string_view(v);
        return std::nullopt;
    }

    auto& entries() const { return kv_; }

private:
    std::vector<std::pair<std::string, std::string>> kv_;
};

/** @brief 递归绑定：prefix.name → 成员（嵌套聚合走子前缀） */
template <class T>
void bind_to(T& out, const properties& p, std::string_view prefix) {
    for_each_member(out, [&](std::string_view name, auto& member) {
        using M = std::remove_reference_t<decltype(member)>;
        std::string key = detail::join_key(prefix, name);
        if constexpr (std::is_aggregate_v<M> && !std::is_array_v<M>
                      && !detail::is_specialization_v<std::vector, M>
                      && !detail::is_specialization_v<std::optional, M>) {
            bind_to(member, p, key);                        // 嵌套聚合 → 子前缀
        } else if constexpr (detail::is_specialization_v<std::optional, M>) {
            if (auto raw = p.get_expanded(key)) {
                typename M::value_type u{};
                detail::convert(*raw, u, key);
                member = std::move(u);
            } else member.reset();                          // 缺键 → nullopt
        } else if constexpr (detail::is_specialization_v<std::vector, M>) {
            if (auto raw = p.get_expanded(key)) {           // 逗号形式优先
                detail::convert(*raw, member, key);
            } else {                                        // 索引键形式
                member.clear();
                for (std::size_t i = 0;; ++i) {
                    if (auto e = p.get_expanded(key + "." + std::to_string(i))) {
                        typename M::value_type u{};
                        detail::convert(*e, u, key);
                        member.push_back(std::move(u));
                    } else break;
                }
            }
        } else {
            if (auto raw = p.get_expanded(key))
                detail::convert(*raw, member, key);         // 缺键 → 保持默认
        }
    });
}

/** @brief 绑定便捷形态：bind<DbProps>(props, "db") */
template <class T>
T bind(const properties& p, std::string_view prefix = {}) {
    T out{};
    bind_to(out, p, prefix);
    return out;
}

} // namespace config
