export module cboot.json;

import std;
import cboot.meta.reflect;    // for_each_member / enum 注册表（全局作用域工具）

/**
 * @brief 轻量 JSON — 值模型 + 递归下降解析 + 反射绑定
 *
 * 结构（kwik-ui 接口/实现分离纪律）：
 *   接口（本文件）：value 值模型 / 模板绑定（get_to / from）/
 *     parse / dump 声明
 *   实现（src/json/json.cpp）：解析器 / 序列化器
 *
 * 设计口径（"简洁高效轻量"）：
 *   - object 用 vector<pair> 保序（避免 map 节点开销；查找线性，
 *     配置/DTO 对象键数小）
 *   - 解析错误带行/列与偏移；嵌套深度上限默认 128
 *   - \uXXXX 含代理对 → UTF-8；dump 紧凑；浮点取最短可往返精度
 *   - 反射绑定：成员名 ≡ JSON 键；未知键忽略、缺失键保持成员默认值
 *     （Spring @ConfigurationProperties 同语义）
 *   - json::codec<T> 可为自定义类型提供 to_value / from_value
 *
 * 支持的成员类型（超出即编译错 fail-loud）：
 *   bool / 整数 / 浮点 / std::string / std::optional<U> / std::vector<U>
 *   / std::map<std::string,V> / std::unordered_map<std::string,V>
 *   / json::value / 枚举（按枚举名）/ 聚合体（递归成员遍历）
 *   / 显式特化 json::codec<T> 的自定义类型
 */

// ── 内部工具（模块作用域，不导出——export template 实例化可用）──────
namespace json::detail {

template <template <class...> class TT, class T>
struct is_specialization : std::false_type {};
template <template <class...> class TT, class... A>
struct is_specialization<TT, TT<A...>> : std::true_type {};
template <template <class...> class TT, class T>
inline constexpr bool is_specialization_v = is_specialization<TT, T>::value;

/** @brief std::string 键的 map / unordered_map（key → JSON object） */
template <class T>
struct string_map_traits {
    static constexpr bool value = false;
};
template <class V, class C, class A>
struct string_map_traits<std::map<std::string, V, C, A>> {
    static constexpr bool value = true;
};
template <class V, class H, class E, class A>
struct string_map_traits<std::unordered_map<std::string, V, H, E, A>> {
    static constexpr bool value = true;
};
template <class T>
inline constexpr bool is_string_map_v = string_map_traits<T>::value;

} // namespace json::detail

export namespace json {

class value;                                                    // 前置：递归容器
using array  = std::vector<value>;
using object = std::vector<std::pair<std::string, value>>;      // 保序

class value {
public:
    value() = default;
    value(std::nullptr_t) {}
    value(bool b) : v_(b) {}
    value(std::int64_t i) : v_(i) {}
    value(int i) : v_(std::int64_t{i}) {}
    value(double d) : v_(d) {}
    value(std::string s) : v_(std::move(s)) {}
    value(std::string_view s) : v_(std::string(s)) {}
    value(const char* s) : v_(std::string(s)) {}
    value(array a) : v_(std::move(a)) {}
    value(object o) : v_(std::move(o)) {}

    bool is_null()   const { return std::holds_alternative<std::monostate>(v_); }
    bool is_bool()   const { return std::holds_alternative<bool>(v_); }
    bool is_int()    const { return std::holds_alternative<std::int64_t>(v_); }
    bool is_double() const { return std::holds_alternative<double>(v_); }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return std::holds_alternative<std::string>(v_); }
    bool is_array()  const { return std::holds_alternative<array>(v_); }
    bool is_object() const { return std::holds_alternative<object>(v_); }

    bool         as_bool()   const { return std::get<bool>(v_); }
    std::int64_t as_int()    const { return std::get<std::int64_t>(v_); }
    double       as_double() const {
        return is_int() ? double(as_int()) : std::get<double>(v_);
    }
    const std::string& as_string() const { return std::get<std::string>(v_); }
    array&        as_array()       { return std::get<array>(v_); }
    const array&  as_array() const { return std::get<array>(v_); }
    object&       as_object()      { return std::get<object>(v_); }
    const object& as_object() const { return std::get<object>(v_); }

    /** @brief 对象键查找（线性；未命中返回 nullptr） */
    const value* find(std::string_view key) const {
        if (!is_object()) return nullptr;
        for (const auto& [k, v] : as_object())
            if (k == key) return &v;
        return nullptr;
    }

    bool operator==(const value&) const = default;

private:
    std::variant<std::monostate, bool, std::int64_t, double,
                 std::string, array, object> v_;
};

class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ── 解析 / 序列化（实现在 src/json/json.cpp）───────────────────────────

/** @brief 解析选项；max_depth 限制对象/数组嵌套层数（防 DoS） */
struct parse_options {
    std::size_t max_depth = 128;
};

value parse(std::string_view src, const parse_options& opts = {});
std::string dump(const value& v);

// ── 自定义类型 codec ────────────────────────────────────────────────────

/**
 * @brief 自定义类型编解码特化点
 *
 *   template <>
 *   struct json::codec<Money> {
 *       static json::value to_value(const Money& m);
 *       static Money from_value(const json::value& v);
 *   };
 *
 * 特化必须在 get_to / from / write_to / get_as 实例化前可见。
 * codec 优先于枚举/聚合体默认规则。
 */
template <class T>
struct codec {};

namespace detail {

template <class T>
concept has_codec = requires(const T& in, const value& v) {
    { json::codec<T>::to_value(in) } -> std::same_as<value>;
    { json::codec<T>::from_value(v) } -> std::same_as<T>;
};
template <class T>
inline constexpr bool has_codec_v = has_codec<T>;

} // namespace detail

// ── 反射绑定（模板：定义须在接口单元，消费方可实例化）──────────────────

/** @brief value → T（类型分派；不支持的成员类型编译错 fail-loud） */
template <class T>
void get_to(const value& v, T& out) {
    if constexpr (std::is_same_v<T, value>) {
        out = v;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!v.is_bool()) throw error("cboot.json: expected bool");
        out = v.as_bool();
    } else if constexpr (std::is_integral_v<T>) {
        if (!v.is_int()) throw error("cboot.json: expected integer");
        out = static_cast<T>(v.as_int());
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!v.is_number()) throw error("cboot.json: expected number");
        out = T(v.as_double());
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!v.is_string()) throw error("cboot.json: expected string");
        out = v.as_string();
    } else if constexpr (detail::is_specialization_v<std::optional, T>) {
        if (v.is_null()) out.reset();
        else {
            typename T::value_type u{};
            get_to(v, u);
            out = std::move(u);
        }
    } else if constexpr (detail::is_specialization_v<std::vector, T>) {
        if (!v.is_array()) throw error("cboot.json: expected array");
        out.clear();
        out.reserve(v.as_array().size());
        for (const auto& e : v.as_array()) {
            typename T::value_type u{};
            get_to(e, u);
            out.push_back(std::move(u));
        }
    } else if constexpr (detail::is_string_map_v<T>) {
        if (!v.is_object()) throw error("cboot.json: expected object");
        out.clear();
        for (const auto& [key, elem] : v.as_object()) {
            typename T::mapped_type u{};
            get_to(elem, u);
            out.emplace(key, std::move(u));
        }
    } else if constexpr (detail::has_codec_v<T>) {
        out = codec<T>::from_value(v);
    } else if constexpr (std::is_enum_v<T>) {
        if (!v.is_string()) throw error("cboot.json: expected enum name string");
        auto n = enum_from_name<T>(v.as_string());
        if (!n) throw error("cboot.json: unknown enum value '" + v.as_string() + "'");
        out = *n;
    } else if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
        if (!v.is_object()) throw error("cboot.json: expected object");
        for_each_member(out, [&](std::string_view name, auto& member) {
            if (const value* child = v.find(name)) get_to(*child, member);
        });
    } else {
        static_assert(sizeof(T) == 0,
                      "cboot.json: unsupported member type "
                      "(supported: bool/int/float/string/optional/vector/enum/aggregate)");
    }
}

/** @brief value → T 便捷形态 */
template <class T>
T get_as(const value& v) {
    if constexpr (detail::has_codec_v<T>) {
        return codec<T>::from_value(v);
    } else {
        T out{};
        get_to(v, out);
        return out;
    }
}

/** @brief T → value（get_to 的逆映射） */
template <class T>
value from(const T& in) {
    if constexpr (std::is_same_v<T, value>) {
        return in;
    } else if constexpr (std::is_same_v<T, bool>) {
        return value(bool(in));
    } else if constexpr (std::is_integral_v<T>) {
        return value(std::int64_t(in));
    } else if constexpr (std::is_floating_point_v<T>) {
        return value(double(in));
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value(in);
    } else if constexpr (detail::is_specialization_v<std::optional, T>) {
        return in ? from(*in) : value(nullptr);
    } else if constexpr (detail::is_specialization_v<std::vector, T>) {
        array out;
        out.reserve(in.size());
        for (const auto& e : in) out.push_back(from(e));
        return value(std::move(out));
    } else if constexpr (detail::is_string_map_v<T>) {
        object out;
        out.reserve(in.size());
        for (const auto& [key, elem] : in)
            out.emplace_back(key, from(elem));
        return value(std::move(out));
    } else if constexpr (detail::has_codec_v<T>) {
        return codec<T>::to_value(in);
    } else if constexpr (std::is_enum_v<T>) {
        return value(std::string(enum_to_name(in)));
    } else if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
        object out;
        for_each_member(in, [&](std::string_view name, const auto& member) {
            out.emplace_back(std::string(name), from(member));
        });
        return value(std::move(out));
    } else {
        static_assert(sizeof(T) == 0,
                      "cboot.json: unsupported member type");
        return value(nullptr);
    }
}

/**
 * @brief 小对象内联缓冲：≤ N 字节不堆分配，超出后溢出到 std::string
 *
 * 用途：HTTP 响应体（多数 JSON / 文本响应小于容量 N）。接口是
 * std::string 的热路径子集（append / push_back / += / assign / ==），
 * 可直接作为 write_to 的输出目标。
 *
 * 语义与 std::string 一致：data() 在 [0, size()) 内连续有效；
 * 内容变化后旧视图失效。不提供 SSO 之外的容量控制。
 */
template <std::size_t N = 256>
class small_buffer {
public:
    small_buffer() = default;
    small_buffer(std::string_view s) { assign(s); }
    small_buffer(const char* s) { assign(s); }

    // 只拷贝 [0, size_)：inline_ 其余字节保持未初始化，避免 memset 成本
    small_buffer(const small_buffer& o) : heap_(o.heap_), size_(o.size_),
                                          spilled_(o.spilled_) {
        if (!spilled_) std::memcpy(inline_.data(), o.inline_.data(), size_);
    }
    small_buffer(small_buffer&& o) noexcept
        : heap_(std::move(o.heap_)), size_(o.size_), spilled_(o.spilled_) {
        if (!spilled_) std::memcpy(inline_.data(), o.inline_.data(), size_);
        o.size_ = 0;
        o.spilled_ = false;
    }
    small_buffer& operator=(const small_buffer& o) {
        if (this == &o) return *this;
        heap_ = o.heap_;
        size_ = o.size_;
        spilled_ = o.spilled_;
        if (!spilled_) std::memcpy(inline_.data(), o.inline_.data(), size_);
        return *this;
    }
    small_buffer& operator=(small_buffer&& o) noexcept {
        if (this == &o) return *this;
        heap_ = std::move(o.heap_);
        size_ = o.size_;
        spilled_ = o.spilled_;
        if (!spilled_) std::memcpy(inline_.data(), o.inline_.data(), size_);
        o.size_ = 0;
        o.spilled_ = false;
        return *this;
    }

    small_buffer& operator=(std::string s) {
        if (s.size() > N) {
            heap_ = std::move(s);
            spilled_ = true;
            size_ = heap_.size();
        } else {
            spilled_ = false;
            size_ = s.size();
            if (size_) std::memcpy(inline_.data(), s.data(), size_);
            heap_.clear();
        }
        return *this;
    }
    small_buffer& operator=(std::string_view s) { assign(s); return *this; }
    small_buffer& operator=(const char* s) { assign(std::string_view(s)); return *this; }

    void assign(std::string_view s) {
        if (spilled_) {
            heap_.assign(s.data(), s.size());
            size_ = heap_.size();
            return;
        }
        size_ = 0;
        append(s.data(), s.size());
    }

    void append(const char* p, std::size_t n) {
        if (spilled_) {
            heap_.append(p, n);
            size_ = heap_.size();
            return;
        }
        if (size_ + n <= N) {
            if (n) std::memcpy(inline_.data() + size_, p, n);
            size_ += n;
            return;
        }
        spill();
        heap_.append(p, n);
        size_ = heap_.size();
    }
    void append(const char* first, const char* last) {
        append(first, static_cast<std::size_t>(last - first));
    }
    void append(std::string_view s) { append(s.data(), s.size()); }

    void push_back(char c) {
        if (spilled_) {
            heap_.push_back(c);
            size_ = heap_.size();
            return;
        }
        if (size_ == N) {
            spill();
            heap_.push_back(c);
            size_ = heap_.size();
            return;
        }
        inline_[size_++] = c;
    }

    small_buffer& operator+=(std::string_view s) { append(s); return *this; }
    small_buffer& operator+=(const char* s) { append(std::string_view(s)); return *this; }

    void clear() {
        size_ = 0;
        if (spilled_) heap_.clear();
    }

    const char* data() const { return spilled_ ? heap_.data() : inline_.data(); }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::string_view view() const { return {data(), size_}; }
    operator std::string_view() const { return view(); }

    friend bool operator==(const small_buffer& a, std::string_view b) {
        return a.view() == b;
    }
    friend bool operator!=(const small_buffer& a, std::string_view b) {
        return a.view() != b;
    }

private:
    void spill() {
        if (spilled_) return;
        heap_.assign(inline_.data(), size_);
        spilled_ = true;
        size_ = heap_.size();
    }

    std::array<char, N> inline_;   // 只读 [0, size_)；拷贝按 size_ 进行
    std::string heap_;             // 仅溢出后使用
    std::size_t size_ = 0;
    bool spilled_ = false;
};

/**
 * @brief JSON 转义写入（流式序列化用；控制字符 → \\uXXXX）
 * 直接追加到 out，不建中间 string（热路径零分配关键）
 */
template <class Out>
inline void write_escaped(Out& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char hex[6] = {'\\', 'u', '0', '0', '0', '0'};
                constexpr char digits[] = "0123456789abcdef";
                hex[4] = digits[(c >> 4) & 0xF];
                hex[5] = digits[c & 0xF];
                out.append(hex, 6);
            } else {
                out.push_back(char(c));
            }
        }
    }
    out.push_back('"');
}

template <class Out, class T>
void write_to(Out& out, const T& in);

/** @brief value → 流式写出（codec 返回值 / 手动组合 value 时用） */
template <class Out>
void write_to(Out& out, const value& v) {
    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.as_bool() ? "true" : "false";
    } else if (v.is_int()) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, v.as_int());
        out.append(buf, r.ptr);
    } else if (v.is_double()) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, v.as_double());
        out.append(buf, r.ptr);
    } else if (v.is_string()) {
        write_escaped(out, v.as_string());
    } else if (v.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& e : v.as_array()) {
            if (!first) out.push_back(',');
            first = false;
            write_to(out, e);
        }
        out.push_back(']');
    } else if (v.is_object()) {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, e] : v.as_object()) {
            if (!first) out.push_back(',');
            first = false;
            write_escaped(out, key);
            out.push_back(':');
            write_to(out, e);
        }
        out.push_back('}');
    }
}

/**
 * @brief 流式序列化：T 直接写入 out（**不建 value 树**）
 *
 * 与 from() 的类型集合一致；供 HTTP 响应等热路径使用。
 * 数字用 to_chars：最短可往返表示、无临时字符串。
 */
template <class Out, class T>
void write_to(Out& out, const T& in) {
    if constexpr (std::is_same_v<T, bool>) {
        out += in ? "true" : "false";
    } else if constexpr (std::is_integral_v<T>) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, in);
        out.append(buf, r.ptr);
    } else if constexpr (std::is_floating_point_v<T>) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, in);
        out.append(buf, r.ptr);
    } else if constexpr (std::is_same_v<T, std::string>) {
        write_escaped(out, in);
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        write_escaped(out, in);
    } else if constexpr (detail::is_specialization_v<std::optional, T>) {
        if (in) write_to(out, *in);
        else out += "null";
    } else if constexpr (detail::is_specialization_v<std::vector, T>) {
        out.push_back('[');
        bool first = true;
        for (const auto& e : in) {
            if (!first) out.push_back(',');
            first = false;
            write_to(out, e);
        }
        out.push_back(']');
    } else if constexpr (detail::is_string_map_v<T>) {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, elem] : in) {
            if (!first) out.push_back(',');
            first = false;
            write_escaped(out, key);
            out.push_back(':');
            write_to(out, elem);
        }
        out.push_back('}');
    } else if constexpr (detail::has_codec_v<T>) {
        write_to(out, codec<T>::to_value(in));
    } else if constexpr (std::is_enum_v<T>) {
        write_escaped(out, enum_to_name(in));
    } else if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>) {
        out.push_back('{');
        bool first = true;
        for_each_member(in, [&](std::string_view name, const auto& member) {
            if (!first) out.push_back(',');
            first = false;
            write_escaped(out, name);
            out.push_back(':');
            write_to(out, member);
        });
        out.push_back('}');
    } else {
        static_assert(sizeof(T) == 0,
                      "cboot.json: unsupported member type (write_to)");
    }
}

} // namespace json
