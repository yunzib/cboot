// 负面用例（预期编译失败）：私有构造器 bean
// 诊断："cboot: bean constructor must be public"
import cboot.core.annotations;
import cboot.core.context;

class Hidden {
    Hidden() = default;                     // 私有构造 → 框架无法构造
public:
    int value = 1;
};

int main() {
    context<Hidden> ctx;
    return ctx.get<Hidden>().value;
}
