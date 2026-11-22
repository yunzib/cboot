// 负面用例（预期编译失败）：循环依赖 A ↔ B
// 构建（-DCBOOT_BUILD_NEGATIVE_TESTS=ON）时观察 consteval 诊断：
//   "cboot: circular dependency detected"
import cboot.core.annotations;
import cboot.core.context;

struct B;
struct A { B& b; A(B& b) : b(b) {} };
struct B { A& a; B(A& a) : a(a) {} };

int main() {
    context<A, B> ctx;   // 环 → 编译错
    return 0;
}
