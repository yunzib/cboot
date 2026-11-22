// 负面用例（预期编译失败）：组件 bean 不能直接放全局命名空间
// 诊断："cboot: component bean 'GlobalBean' must not be a direct member of the global namespace"
import cboot.app;
import std;

struct [[=Bean{}]] GlobalBean {};

struct bad_component {
    using beans = type_list<GlobalBean>;
    using dependencies = type_list<>;
};

struct App : application<App> {
    using components = type_list<bad_component>;
};

int main() {
    using Beans = app_beans_t<App>;
    (void)sizeof(Beans);
}
