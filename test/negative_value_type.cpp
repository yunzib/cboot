// 负面用例（预期编译失败）：[[=Value{}]] 只支持标量
// 诊断："cboot: [[=Value{}]] supports bool/integral/floating/string/enum"
import cboot.core.annotations;
import cboot.core.context;
import std;

struct [[=Bean{}]] BadValue {
    BadValue([[=Value{"app.items"}]] std::vector<int> items) {}
};

int main() {
    context<BadValue> ctx;
}
