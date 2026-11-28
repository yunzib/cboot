/**
 * @file lifecycle_aspect_smoke.cpp
 * @brief P2-4 生命周期切面冒烟测试
 *
 * 覆盖：
 *   - PostConstruct / PreDestroy 前后拦截，逆拓扑序销毁
 *   - call_info：bean / method / point
 *   - 切面短路：不调 next() 则生命周期方法不执行
 *   - PostConstruct 抛异常（切面或方法本体）→ 容器构造失败
 *   - PreDestroy 异常隔离：不中断其余 bean 的销毁
 *   - App::lifecycle_aspects → app_lifecycle_t，引导链可编译
 */
import cboot.app;
import std;

namespace {

std::vector<std::string> g_log;    // bean 本体与切面共用的顺序日志

// ── 切面：记录 before / after，并校验连接点 ────────────────────────────

struct LifecycleTrace {
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        const char* phase = ci.point == join_point::post_construct ? "init"
                         : ci.point == join_point::pre_destroy   ? "destroy"
                                                                    : "unknown";
        const std::string who = std::string(ci.bean) + "." + std::string(ci.method)
                              + ":" + phase;
        g_log.push_back("before:" + who);
        next();
        g_log.push_back("after:" + who);
    }
};

/** @brief 在 PostConstruct 之前抛出，且不调 next()：初始化被拦截 */
struct BlockInit {
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        if (ci.point == join_point::post_construct)
            throw std::runtime_error("init-blocked:" + std::string(ci.bean));
        next();
    }
};

/** @brief PostConstruct 不调 next()：初始化被跳过，且不抛异常 */
struct SkipInit {
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        if (ci.point != join_point::post_construct) next();
    }
};

/** @brief 记录生命周期方法抛出的异常后原样抛出（切面能看到异常） */
struct ObserveThrow {
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        try {
            next();
        } catch (...) {
            g_log.push_back("saw-throw:" + std::string(ci.bean));
            throw;
        }
    }
};

/** @brief 在 PreDestroy 之后抛出：验证销毁阶段的异常隔离 */
struct ThrowAfterDestroy {
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        next();
        if (ci.point == join_point::pre_destroy)
            throw std::runtime_error("after-destroy:" + std::string(ci.bean));
    }
};

// ── Bean ──────────────────────────────────────────────────────────────

struct Repo {
    [[=PostConstruct{}]] void open() { g_log.push_back("body:Repo.open"); }
    [[=PreDestroy{}]] void close() { g_log.push_back("body:Repo.close"); }
};

struct Svc {
    explicit Svc(Repo& repo) : repo_(repo) {}    // 依赖 Repo：Repo 先构造、后销毁
    [[=PostConstruct{}]] void init() { g_log.push_back("body:Svc.init"); }
    [[=PreDestroy{}]] void shutdown() { g_log.push_back("body:Svc.shutdown"); }
    Repo& repo_;
};

struct InitThrows {
    [[=PostConstruct{}]] void init() { throw std::runtime_error("init-failed"); }
};

struct BlockedInit {
    [[=PostConstruct{}]] void init() { g_log.push_back("body:BlockedInit.init"); }
};

struct AfterBad {                                // 清单序靠前 → 后销毁
    [[=PreDestroy{}]] void close() { g_log.push_back("body:AfterBad.close"); }
};

struct BadClose {                                // 清单序靠后 → 先销毁，且抛出
    [[=PreDestroy{}]] void close() {
        g_log.push_back("body:BadClose.close");
        throw std::runtime_error("close-failed");
    }
};

bool check(const char* name, const std::vector<std::string>& expected) {
    if (g_log == expected) return true;
    std::println("{}: MISMATCH", name);
    for (const auto& line : g_log) std::println("  got      : {}", line);
    for (const auto& line : expected) std::println("  expected : {}", line);
    return false;
}

} // namespace

// ── App 声明 → 生命周期切面清单 ────────────────────────────────────────

struct LifecycleApp : application<LifecycleApp> {
    static constexpr auto lifecycle_aspects =
        std::tuple<LifecycleTrace, ThrowAfterDestroy>{};
};

struct PlainApp : application<PlainApp> {};

static_assert(std::is_same_v<app_lifecycle_t<LifecycleApp>,
                             type_list<LifecycleTrace, ThrowAfterDestroy>>);
static_assert(std::is_same_v<app_lifecycle_t<PlainApp>, type_list<>>);

int main(int argc, char**) {
    // ── 1. 前后拦截：每 bean 构造后立即 init，析构逆拓扑序 ─────────────
    {
        g_log.clear();
        {
            context<Repo, Svc> ctx(with_lifecycle_aspects, nullptr,
                                   LifecycleTrace{});
        }
        if (!check("interception", {
                "before:Repo.open:init", "body:Repo.open", "after:Repo.open:init",
                "before:Svc.init:init", "body:Svc.init", "after:Svc.init:init",
                "before:Svc.shutdown:destroy", "body:Svc.shutdown",
                "after:Svc.shutdown:destroy",
                "before:Repo.close:destroy", "body:Repo.close",
                "after:Repo.close:destroy",
            }))
            return 1;
    }

    // ── 2. 默认构造：不经过切面，行为不变 ───────────────────────────────
    {
        g_log.clear();
        {
            context<Repo, Svc> ctx;
        }
        if (!check("default", {
                "body:Repo.open", "body:Svc.init",
                "body:Svc.shutdown", "body:Repo.close",
            }))
            return 2;
    }

    // ── 3. 切面短路：不调 next() → PostConstruct 不执行，且不抛异常 ─────
    {
        g_log.clear();
        {
            context<BlockedInit> ctx(with_lifecycle_aspects, nullptr, SkipInit{});
        }
        if (!check("short-circuit", {})) return 3;
    }

    // ── 4. PostConstruct 抛异常 → 容器构造失败（切面抛 / 方法本体抛）──
    {
        g_log.clear();
        bool threw = false;
        try {
            context<BlockedInit> ctx(with_lifecycle_aspects, nullptr,
                                     BlockInit{}, ThrowAfterDestroy{});
        } catch (const std::exception& e) {
            threw = std::string_view(e.what()).starts_with("init-blocked:");
        }
        if (!threw) return 4;
    }
    {
        g_log.clear();
        bool threw = false;
        try {
            context<InitThrows> ctx(with_lifecycle_aspects, nullptr, ObserveThrow{});
        } catch (const std::exception& e) {
            threw = std::string_view(e.what()) == "init-failed";
        }
        if (!threw) return 5;
        if (!check("init-throw-propagates", {"saw-throw:InitThrows"})) return 6;
    }

    // ── 5. PreDestroy 异常隔离：抛出的 bean 不阻断其余 bean 的销毁 ──────
    {
        g_log.clear();
        {
            context<AfterBad, BadClose> ctx;      // 方法本体抛出
        }
        if (!check("destroy-isolated-body",
                   {"body:BadClose.close", "body:AfterBad.close"}))
            return 7;
    }
    {
        g_log.clear();
        {
            // 切面抛出：每个 bean 销毁后都抛一次，两个方法本体都执行到
            context<AfterBad, BadClose> ctx(with_lifecycle_aspects, nullptr,
                                            ThrowAfterDestroy{});
        }
        if (!check("destroy-isolated-aspect",
                   {"body:BadClose.close", "body:AfterBad.close"}))
            return 8;
    }

    // argc>200 才运行：实例化 application::run，验证 App::lifecycle_aspects
    // 贯通引导链（容器构造 → 服务 → 析构）。
    if (argc > 200) return LifecycleApp{}.run();

    std::println("LIFECYCLE ASPECT SMOKE: PASS");
    return 0;
}
