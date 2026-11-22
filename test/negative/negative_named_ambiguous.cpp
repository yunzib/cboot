// 负面用例（预期编译失败）：多候选且无默认实例，未用 [[=Named{}]] 限定
// 诊断："cboot: 'Svc' has 2 candidates for 'DataSource' — annotate the parameter ..."
import cboot.core.annotations;
import cboot.core.context;
import std;

struct DataSource {
    virtual ~DataSource() = default;
};

struct [[=Bean{"primary"}]] Primary : DataSource {};
struct [[=Bean{"replica"}]] Replica : DataSource {};

struct [[=Bean{}]] Svc {
    Svc(DataSource&) {}
};

int main() {
    context<Primary, Replica, Svc> ctx;
}
