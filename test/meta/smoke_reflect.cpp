// cboot.meta.reflect 冒烟测试
// 验证：① 模块链路（import + BMI 消费 + -freflection 传递）
//       ② R1 反射三件套 + for_each_member（全局模块结构体 = 最不利消费场景）
//       ③ enum 注册表（枚举名/值互查）
//       ④ R2 探针结果回显（configure 期探测的编译定义）
import cboot.meta.reflect;
import std;

struct DbProps {                        // 用户侧结构体（全局模块，模拟真实消费）
    std::string url{"localhost"};
    int         pool_size{4};
    bool        readonly{false};
};

enum class Scope { Singleton, Prototype };

#if CBOOT_HAS_ANNOTATION_VALUE
// P3394 注解演示（R4 形态：值表达式 + class-key 后 + annotations_of_with_type）
struct Tag { int id; };
struct [[=Tag{7}]] AnnotatedBean {};
consteval int bean_tag() { return annotation_of<Tag>(^^AnnotatedBean)->id; }
static_assert(bean_tag() == 7);
consteval int member_tag() {
    struct S { [[=Tag{3}]] int priority; };
    for (auto m : std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current()))
        if (auto t = annotation_of<Tag>(m)) return t->id;
    return -1;
}
static_assert(member_tag() == 3);
#endif

#if CBOOT_HAS_EXPANSION
// P1306 展开语句演示：members_array 适配器 + template for 编译期取成员名
consteval auto db_member_names() {
    std::array<std::string_view, member_count<DbProps>()> out{};
    std::size_t i = 0;
    template for (constexpr auto m : members_array<^^DbProps>()) {
        out[i++] = std::meta::identifier_of(m);
    }
    return out;
}
#endif

int main() {
    // ── 1. for_each_member：名字 + 值遍历 ──
    DbProps p;
    std::vector<std::string_view> names;
    for_each_member(p, [&](std::string_view name, auto&& value) {
        names.push_back(name);
        std::println("  {} = {}", name, value);
    });

    // ── 2. const 视图遍历 ──
    int const_total = 0;
    for_each_member(std::as_const(p), [&](std::string_view, auto&&) { ++const_total; });

    // ── 3. member_count / members_array 编译期断言 ──
    static_assert(member_count<DbProps>() == 3);
    constexpr auto mems = members_array<^^DbProps>();    // array 适配器可 constexpr 持有
    static_assert(mems.size() == 3);
#if CBOOT_HAS_EXPANSION
    static_assert(db_member_names()[0] == "url" && db_member_names()[2] == "readonly");
    std::println("expansion demo (template for): PASS");
#endif

    // ── 4. enum 注册表 ──
    static_assert(enum_names<Scope>().size() == 2);
    constexpr std::string_view proto_name = enum_to_name(Scope::Prototype);
    static_assert(proto_name == "Prototype");

#if CBOOT_HAS_ANNOTATION_VALUE
    std::println("annotation demo ([[=Tag{{7}}]] + extract): PASS");
#endif
    // ── 5. R2 探针结果回显 ──
    std::println("probes: annotations={} annotation_value={} expansion={} ctors={}",
                 CBOOT_HAS_ANNOTATIONS, CBOOT_HAS_ANNOTATION_VALUE,
                 CBOOT_HAS_EXPANSION, CBOOT_HAS_CTORS);

    const bool ok = names.size() == 3
        && names[0] == "url" && names[1] == "pool_size" && names[2] == "readonly"
        && const_total == 3
        && enum_from_name<Scope>("Singleton") == Scope::Singleton
        && !enum_from_name<Scope>("__no_such__").has_value();

    std::println("{}", ok ? "SMOKE REFLECT: PASS" : "SMOKE REFLECT: FAIL");
    return ok ? 0 : 1;
}
