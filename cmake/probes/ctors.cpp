// 探针：构造器反射（members_of + is_constructor + parameters_of 参数枚举）
// GCC16.2 口径：parameters_of 为单参重载（无 access_context 版本）
#include <meta>
struct X { X(int, double) {} };
consteval bool two_params() {
    for (auto m : std::meta::members_of(^^X, std::meta::access_context::current())) {
        if (std::meta::is_constructor(m))
            return std::meta::parameters_of(m).size() == 2;
    }
    return false;
}
static_assert(two_params());
int main() { return 0; }
