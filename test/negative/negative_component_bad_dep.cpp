// 负面用例（预期编译失败）：dependencies 只能是组件
// 诊断："cboot: component 'bad_component' depends on 'NotComponent' which is not a Component"
import cboot.app;
import std;

struct NotComponent {};

struct bad_component {
    using beans = type_list<>;
    using dependencies = type_list<NotComponent>;
};

struct App : application<App> {
    using components = type_list<bad_component>;
};

int main() {
    using Beans = app_beans_t<App>;
    (void)sizeof(Beans);
}
