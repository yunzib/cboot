// 负面用例（预期编译失败）：非法 cron 表达式
// 诊断："cboot: invalid cron expression '61 * * * *': field value out of range"
import cboot.core.annotations;
import cboot.core.context;
import cboot.schedule;
import std;

class [[=Bean{}]] Bad {
public:
    [[=Scheduled{.expression = "61 * * * *"}]] void task() {}
};

int main() {
    context<Bad> ctx;
    hook_scope<decltype(ctx), schedule_hook> hooks(ctx);
}
