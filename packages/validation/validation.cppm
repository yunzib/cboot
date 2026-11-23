/**
 * @file validation.cppm
 * @brief DTO 字段校验 — 注解 + 编译期扫描 + 运行期错误聚合
 *
 * 设计边界：
 *   - 只做 DTO 字段级语义校验，不依赖 HTTP / JSON 模块
 *   - 注解是 structural 值，可放在 aggregate 字段上
 *   - validate<T>() 在编译期展开成员，运行期返回 field_error 列表
 *   - 第一版支持 NotBlank（std::string）与 Range（整数/浮点）
 *
 * 不提供嵌套 DTO 递归、自定义 validator 注册表或国际化；
 * 这些能力等到出现真实消费者再扩展。
 */
export module cboot.validation;

import std;
import cboot.core.annotations;   // bean_name
import cboot.meta.reflect;       // member_count / nth_member / annotation_of

export namespace validation {

/** @brief 非空字符串；message 为空时使用默认文案 */
struct NotBlank {
    bean_name message{};
};

/** @brief 数值范围；message 为空时自动生成默认文案 */
struct Range {
    std::int64_t min{};
    std::int64_t max{};
    bean_name message{};
};

/** @brief 字段级校验错误；field 使用成员名 */
struct field_error {
    std::string field;
    std::string message;
};

/** @brief 校验失败异常；携带全部字段错误，供 HTTP 层转 400 */
class invalid : public std::runtime_error {
public:
    explicit invalid(std::vector<field_error> errors)
        : std::runtime_error("validation failed"), errors_(std::move(errors)) {}

    /** @brief 全部字段错误；生命周期与异常对象一致 */
    const std::vector<field_error>& errors() const noexcept { return errors_; }

private:
    std::vector<field_error> errors_;
};

namespace detail {

inline bool is_blank(std::string_view value) {
    return value.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos;
}

template <class T, std::size_t I>
void validate_member(const T& dto, std::vector<field_error>& out) {
    using M = std::remove_cvref_t<
        type_of_t<std::meta::type_of(nth_member<T, I>())>>;
    constexpr std::meta::info member = nth_member<T, I>();
    constexpr auto not_blank = annotation_of<NotBlank>(member);
    constexpr auto range = annotation_of<Range>(member);

    if constexpr (not_blank.has_value()) {
        static_assert(std::is_same_v<M, std::string>,
                      "cboot.validation: NotBlank supports std::string fields");
        if (is_blank(dto.[:member:])) {
            const std::string message = not_blank->message.empty()
                ? "must not be blank"
                : std::string(not_blank->message.view());
            out.push_back({std::string(nth_member_name<T, I>()), message});
        }
    }

    if constexpr (range.has_value()) {
        static_assert((std::is_integral_v<M> && !std::is_same_v<M, bool>)
                          || std::is_floating_point_v<M>,
                      "cboot.validation: Range supports integer/floating fields");
        static_assert(range->min <= range->max,
                      "cboot.validation: Range min must be <= max");
        const auto& value = dto.[:member:];
        if (value < range->min || value > range->max) {
            const std::string message = range->message.empty()
                ? "must be between " + std::to_string(range->min)
                    + " and " + std::to_string(range->max)
                : std::string(range->message.view());
            out.push_back({std::string(nth_member_name<T, I>()), message});
        }
    }
}

} // namespace detail

/**
 * @brief 校验 aggregate DTO 的字段注解
 *
 * 返回全部错误，按成员声明序排列；没有错误时返回空 vector。
 * 不支持 DTO 嵌套递归；嵌套字段的注解目前不会自动展开。
 */
template <class T>
std::vector<field_error> validate(const T& dto) {
    static_assert(std::is_aggregate_v<T> && !std::is_array_v<T>,
                  "cboot.validation: validate<T> requires an aggregate DTO");
    std::vector<field_error> out;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (detail::validate_member<T, I>(dto, out), ...);
    }(std::make_index_sequence<member_count<T>()>{});
    return out;
}

/** @brief 校验并在失败时抛 invalid；web body 绑定使用这个入口 */
template <class T>
void validate_or_throw(const T& dto) {
    auto errors = validate(dto);
    if (!errors.empty()) throw invalid(std::move(errors));
}

} // namespace validation
