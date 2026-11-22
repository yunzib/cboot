// 负面用例（预期编译失败）：fixed_delay_ms 与 expression 不能同时设置
// 诊断："cboot: scheduled 'Bad.task' requires exactly one of fixed_delay_ms or expression"
import cboot.core.annotations;
import cboot.core.context;
import cboot.schedule;
import std;

class [[=Bean{}]] Bad {
public:
    [[=Scheduled{.fixed_delay_ms = 100, .expression = "* * * * * *"}]]
    void task() {}
};

int main() {
    context<Bad> ctx;
    hook_scope<decltype(ctx), schedule_hook> hooks(ctx);
}
