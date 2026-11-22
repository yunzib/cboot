// cboot.core.context 冒烟测试
// 验证：① 注解式 Bean 注册与元数据 ② 编译期拓扑排序（构造顺序）
//       ③ 构造器注入（引用直连/同一实例）④ PostConstruct/PreDestroy 时序
import cboot.core.annotations;
import cboot.core.context;
import cboot.meta.reflect;   // annotation_of（模块导入不传递，需显式）
import std;

namespace app {

std::vector<std::string> events;   // 全局事件日志（顺序断言用）

// ── Bean 定义（Spring 同形写法）──

struct [[=Bean{"repo"}]] UserRepository {
    UserRepository() { events.push_back("ctor:repo"); }
    int find(int id) const { return id * 2; }
    [[=PreDestroy{}]] void close() { events.push_back("destroy:repo"); }
};

struct AuditLog {                                // 无注解 bean（清单即注册）
    AuditLog() { events.push_back("ctor:audit"); }
};

struct [[=Bean{"userService"}]] UserService {
    UserService(UserRepository& repo, AuditLog& audit)
        : repo_(repo), audit_(audit) { events.push_back("ctor:userService"); }
    [[=PostConstruct{}]] void warm_up() { events.push_back("post:userService"); }
    int serve(int id) { return repo_.find(id); }
    UserRepository& repo_;
    AuditLog& audit_;
};

struct OrderService {                            // 无注解 bean（显式清单即登记）
    OrderService(UserService& svc) : svc_(svc) { events.push_back("ctor:order"); }
    [[=PostConstruct{}]] void ready() { events.push_back("post:order"); }
    UserService& svc_;
};

} // namespace app

int main() {
    // ── 1. 注解元数据（annotation_of 全链路，全局作用域工具）──
    static_assert(annotation_of<Bean>(^^app::UserService)->name.view()
                  == "userService");
    static_assert(annotation_of<Bean>(^^app::UserRepository)->name.view()
                  == "repo");

    {
        // ── 2. 容器构建（编译期：拓扑排序 + 依赖校验已通过）──
        context<app::UserRepository, app::AuditLog,
                       app::UserService, app::OrderService> ctx;

        // ── 3. 注入正确性：依赖者可用 + 同一实例（引用直连）──
        if (ctx.get<app::OrderService>().svc_.serve(21) != 42) {
            std::println("serve() wrong result");
            return 1;
        }
        if (&ctx.get<app::UserService>().repo_ != &ctx.get<app::UserRepository>()
            || &ctx.get<app::UserService>().audit_ != &ctx.get<app::AuditLog>()) {
            std::println("injected refs are not the same instances");
            return 1;
        }
    }   // 析构触发 PreDestroy

    // ── 4. 时序断言 ──
    // 拓扑序（Kahn 同层取清单序）：repo → audit → userService → order
    // 每 bean 构造后立即 PostConstruct；PreDestroy 逆序
    const std::vector<std::string> expected = {
        "ctor:repo", "ctor:audit",
        "ctor:userService", "post:userService",
        "ctor:order", "post:order",
        "destroy:repo",
    };
    bool ok = app::events.size() == expected.size();
    if (ok) {
        for (std::size_t i = 0; i < app::events.size(); ++i) {
            if (app::events[i] != expected[i]) { ok = false; break; }
        }
    }
    if (!ok) {
        std::println("event sequence mismatch:");
        for (auto& e : app::events) std::println("  {}", e);
        return 1;
    }
    std::println("CONTEXT SMOKE: PASS");
    return 0;
}
