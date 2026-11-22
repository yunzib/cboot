// 负面用例（预期编译失败）：缺失依赖（Cache 不在清单）
// 构建时观察 consteval 诊断：
//   "cboot: unsatisfied dependency (type not in bean list)"
import cboot.core.annotations;
import cboot.core.context;

struct Cache { int size{}; };
struct Service { Cache& cache; Service(Cache& c) : cache(c) {} };

int main() {
    context<Service> ctx;    // Cache 未登记 → 编译错
    return 0;
}
