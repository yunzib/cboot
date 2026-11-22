// 负面用例（预期编译失败）：多个构造器且未标注 [[=Inject{}]]
// 诊断："cboot: 'Multi' has 2 constructors — annotate the injected one with [[=Inject{}]]"
import cboot.core.context;

struct Multi {
    Multi() = default;
    Multi(int x) : v(x) {}
    int v{};
};

int main() {
    context<Multi> ctx;
    return ctx.get<Multi>().v;
}
