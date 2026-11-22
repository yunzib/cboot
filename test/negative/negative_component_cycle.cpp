// 负面用例（预期编译失败）：组件依赖环
// 诊断："cboot: component dependency cycle: A -> B -> A"
import cboot.app;
import std;

struct B;

struct A {
    using beans = type_list<>;
    using dependencies = type_list<B>;
};

struct B {
    using beans = type_list<>;
    using dependencies = type_list<A>;
};

struct App : application<App> {
    using components = type_list<A>;
};

int main() {
    using Beans = app_beans_t<App>;
    (void)sizeof(Beans);
}
