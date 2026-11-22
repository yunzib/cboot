// 探针：注解取值全链路（带值注解 [[=Anno{v}]] + annotations_of_with_type + extract<T>）
// GCC16.2（P3394R4）口径：与 annotations 探针同形态，额外验证 extract 取回值
#include <meta>
namespace probe { struct Anno { int v; }; }

struct [[=probe::Anno{42}]] Y {};

consteval bool value_ok() {
    for (auto a : std::meta::annotations_of_with_type(^^Y, ^^probe::Anno))
        if (std::meta::extract<probe::Anno>(a).v == 42) return true;
    return false;
}
static_assert(value_ok());
int main() { return 0; }
