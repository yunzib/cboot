// 负面用例（预期编译失败）：监听方法必须恰一个事件参数
// 诊断："cboot: listener 'Bad.on' must take exactly one event parameter"
import cboot.core.annotations;
import cboot.core.context;
import std;

struct OrderPlaced {
    int id{};
};

class [[=Bean{}]] Bad {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced& e, int extra) {}
};

int main() {
    context<Bad> ctx;
}
