// 探针：P1306 展开语句（template for + array 适配器遍历成员）
// GCC16 纪律：vector<info> 非常量表达式，不能直接作 template for 的 range——
// 须经 members_array 适配器转 std::array<info, N>（本形态已在 GCC16.2 验证）
#include <meta>
#include <array>
struct X { int a; int b; };
template <std::meta::info R>
consteval auto members_array() {
    constexpr std::size_t n = std::meta::nonstatic_data_members_of(R, std::meta::access_context::current()).size();
    std::array<std::meta::info, n> arr{};
    for (std::size_t i = 0; i < n; ++i)
        arr[i] = std::meta::nonstatic_data_members_of(R, std::meta::access_context::current())[i];
    return arr;
}
consteval std::size_t n_members() {
    std::size_t n = 0;
    template for (constexpr auto m : members_array<^^X>()) { ++n; }
    return n;
}
static_assert(n_members() == 2);
int main() { return 0; }
