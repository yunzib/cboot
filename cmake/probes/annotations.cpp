// 探针：P3394 用户注解（类型/成员/成员函数 + annotations_of_with_type 检索）
// GCC16.2（P3394R4）口径：
//   - 注解值必须是值表达式（[[=Anno{}]]），裸类型名 [[=Anno]] 是语法错误
//   - 类型注解须置于 class-key 之后：struct [[=Anno{}]] X {}
//   - 检索走 annotations_of_with_type(r, ^^Anno)——annotations_of 返回的
//     info 上 identifier_of(type_of(a)) 会 throw（has_identifier false），
//     手动 type_of 匹配路不通
#include <meta>
namespace probe { struct Anno {}; }

struct [[=probe::Anno{}]] X {};

consteval bool type_anno() {
    return std::meta::annotations_of_with_type(^^X, ^^probe::Anno).size() == 1;
}
consteval bool member_anno() {
    struct S { [[=probe::Anno{}]] int field; };
    for (auto m : std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current()))
        if (std::meta::annotations_of_with_type(m, ^^probe::Anno).size() == 1) return true;
    return false;
}
consteval bool fn_anno() {
    struct W { [[=probe::Anno{}]] void handler(int); };
    for (auto m : std::meta::members_of(^^W, std::meta::access_context::current()))
        if (std::meta::annotations_of_with_type(m, ^^probe::Anno).size() == 1) return true;
    return false;
}
static_assert(type_anno() && member_anno() && fn_anno());
int main() { return 0; }
