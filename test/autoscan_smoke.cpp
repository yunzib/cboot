// 自动发现（注解即注册）冒烟测试
//
// 验证：
//   ① 全局扫描收集 [[=Bean{}]] 服务与带路由注解的控制器（无需清单）
//   ② 非注解类型（DTO）不被卷入
//   ③ 扫描清单可直接构造容器：DI + 路由派发正常
//   ④ run(port[, aspects...]) 引导链可编译（运行不触发）
import cboot.aop;
import cboot.core.annotations;
import cboot.core.context;
import cboot.json;
import cboot.meta.reflect;
import cboot.web.http;
import cboot.web.routing;
import cboot.app;
import std;

// ── 用户代码：只加注解 ──

struct [[=Bean{"repo"}]] Repo {
    int find(int id) { return id * 2; }
};

struct [[=Bean{}]] Svc {
    Svc(Repo& repo) : repo_(repo) {}
    int calc(int id) { return repo_.find(id); }
    Repo& repo_;
};

struct [[=Bean{}]] Api {                 // 控制器也是 bean：显式标注
    Api(Svc& svc) : svc_(svc) {}
    [[=Get{"/v/{id}"}]]
    Response value(int id) { return json_response(json::value(svc_.calc(id))); }
    Svc& svc_;
};

struct Dto {                             // 非 bean：不应出现在清单
    int x{};
};

// 应用形态（身份 + 配置载体；aspects 可选）
struct NoopAspect {
    template <class Next>
    Response around(const call_info&, Next&& next) { return next(); }
};

struct TestApp : application<TestApp> {
    static constexpr unsigned short default_port = 19001;
    static constexpr auto aspects = std::tuple<NoopAspect>{};
};

template <class L, class T>
struct contains;
template <class... Ts, class T>
struct contains<type_list<Ts...>, T>
    : std::bool_constant<(std::is_same_v<Ts, T> || ...)> {};

int main(int argc, char**) {
    // ── 0. 注解确实注册（防裸类型名 [[=Bean]] 被 GCC 静默丢弃）──
    static_assert(annotation_of<Bean>(^^Repo).has_value());
    static_assert(annotation_of<Bean>(^^Svc).has_value());

    // ── 1. 扫描清单内容 ──
    using beans = auto_beans_t<>;
    static_assert(contains<beans, Repo>::value);
    static_assert(contains<beans, Svc>::value);
    static_assert(contains<beans, Api>::value);
    static_assert(!contains<beans, Dto>::value);

    // ── 2. 清单 → 容器：DI 与路由 ──
    using Ctx = context_for<beans>;
    Ctx ctx;
    if (ctx.get<Svc>().calc(21) != 42) return 1;

    router r;
    r.mount(ctx.get<Api>());
    auto resp = r.dispatch("GET", "/v/5", "");
    if (resp.status != 200 || json::parse(resp.body).as_int() != 10) return 2;

    // ── 3. 引导链编译验证（argc>100 才运行，测试不触发）──
    if (argc > 100) return run(19000);        // 自由函数形态编译验证
    if (argc > 200) return TestApp{}.run();   // application 形态编译验证

    std::println("AUTOSCAN SMOKE: PASS");
    return 0;
}
