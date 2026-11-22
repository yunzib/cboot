// 负面用例（预期编译失败）：定时方法不能带参数
// 诊断："cboot: scheduled 'Bad.task' must take no parameters"
import cboot.core.annotations;
import cboot.core.context;
import std;

class [[=Bean{}]] Bad {
public:
    [[=Scheduled{100}]] void task(int x) {}
};

int main() {
    context<Bad> ctx;
}
