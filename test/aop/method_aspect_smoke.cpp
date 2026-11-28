/**
 * @file method_aspect_smoke.cpp
 * @brief P2-5 方法级切面选择冒烟测试
 *
 * 覆盖：
 *   - 切面是 Bean：可注入依赖，实例在路由间共享
 *   - [[=web::UseAspect{"name"}]] 方法级 / 控制器级，重名只生效一次
 *   - 未标注的路由不经过切面
 *   - 层次：全局切面（最外）→ 类级 → 方法级（最内）
 *   - 全局切面的值状态在 bean 的全部路由间共享（用了 UseAspect 也不例外）
 *   - 切面短路
 *   - 未注册的名字 / 无注册表 mount → 编译错（见 negative/）
 *   - profile 未激活的切面 Bean → 启动期报错
 *   - application 引导链可编译
 */
import cboot.app;
import std;

std::vector<std::string> g_log;      // 顺序日志（全局切面与切面 Bean 共用）

/** @brief 被切面注入的普通 Bean：证明切面可以依赖注入 */
class [[=Bean{}]] HitCounter {
public:
    int hits = 0;
};

/** @brief 类 "tag"：计数、记录 call_info、给响应打头 */
class [[=Bean{}]] [[=web::Aspect{"tag"}]] Tagging {
public:
    explicit Tagging(HitCounter& counter) : counter_(counter) {}

    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        if (ci.point != join_point::method) g_log.push_back("bad-point");
        ++counter_.hits;
        g_log.push_back("tag:" + std::string(ci.bean) + "." + std::string(ci.method));
        Response r = next();
        r.header("X-Tag", "1");
        return r;
    }

private:
    HitCounter& counter_;
};

/** @brief 名 "deny"：直接短路 */
class [[=Bean{}]] [[=web::Aspect{"deny"}]] Deny {
public:
    template <class Next>
    Response around(const call_info&, Next&&) {
        g_log.push_back("deny");
        Response r = Response::text("denied");
        r.status = 403;
        return r;
    }
};

/** @brief 只有 [[=web::Aspect{"pro"}]]、prod 专属：默认 profile 下不可用 */
class [[=Bean{}]] [[=Profile{"prod"}]] [[=web::Aspect{"pro"}]] ProdOnly {
public:
    template <class Next>
    Response around(const call_info&, Next&& next) { return next(); }
};

/** @brief 没有 [[=web::Aspect]]：形状像切面，但不可被选择 */
class [[=Bean{}]] UnnamedAspect {
public:
    template <class Next>
    Response around(const call_info&, Next&& next) { return next(); }
};

class [[=Bean{}]] Api {
public:
    [[=Get{"/plain"}]]
    Response plain() { return Response::text("plain"); }

    [[=Get{"/tagged"}]]
    [[=web::UseAspect{"tag"}]]
    Response tagged() { return Response::text("tagged"); }

    [[=Get{"/denied"}]]
    [[=web::UseAspect{"tag"}]] [[=web::UseAspect{"deny"}]]
    Response denied() { return Response::text("never"); }

    [[=Get{"/dup"}]]
    [[=web::UseAspect{"tag"}]] [[=web::UseAspect{"tag"}]]   // 重复：只生效一次
    Response dup() { return Response::text("dup"); }
};

/** @brief 控制器级 "tag"：对全部路由生效；方法级追加 */
class [[=Bean{}]] [[=web::UseAspect{"tag"}]] AdminApi {
public:
    [[=Get{"/admin"}]]
    Response admin() { return Response::text("admin"); }

    [[=Get{"/admin/all"}]]
    [[=web::UseAspect{"deny"}]]
    Response all() { return Response::text("never"); }
};

class [[=Bean{}]] ProdApi {
public:
    [[=Get{"/prod"}]]
    [[=web::UseAspect{"pro"}]]
    Response prod() { return Response::text("prod"); }
};

/** @brief 全局切面（App::aspects 形态的值对象）：最外层；值状态在 bean 的全部路由间共享 */
struct EnterLog {
    int calls = 0;      // 值状态：验证全局切面每个 bean 只有一份，不随路由复制

    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        g_log.push_back("enter:" + std::string(ci.bean) + "." + std::string(ci.method)
                        + "#" + std::to_string(++calls));
        return next();
    }
};

struct AspectApp : application<AspectApp> {};

bool has_header(const Response& r, std::string_view name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return true;
    return false;
}

bool check(const char* name, const std::vector<std::string>& expected) {
    if (g_log == expected) return true;
    std::println("{}: MISMATCH", name);
    for (const auto& line : g_log) std::println("  got      : {}", line);
    for (const auto& line : expected) std::println("  expected : {}", line);
    return false;
}

int main(int argc, char**) {
    context<HitCounter, Tagging, Deny, ProdOnly, UnnamedAspect, Api, AdminApi,
            ProdApi> ctx;
    router r;
    mount_routes(r, ctx, ctx.get<Api>());
    mount_routes(r, ctx, ctx.get<AdminApi>(), EnterLog{});

    // ── 1. 未标注的路由：不经过切面 ─────────────────────────────────────
    g_log.clear();
    {
        const Response resp = r.dispatch("GET", "/plain", "");
        if (resp.status != 200 || std::string_view(resp.body) != "plain") return 1;
        if (has_header(resp, "X-Tag")) return 2;
        if (ctx.get<HitCounter>().hits != 0) return 3;
        if (!g_log.empty()) return 4;
    }

    // ── 2. 方法级选择：切面 Bean 生效（计数 = 注入的依赖被改写）─────────
    g_log.clear();
    {
        const Response resp = r.dispatch("GET", "/tagged", "");
        if (resp.status != 200 || !has_header(resp, "X-Tag")) return 5;
        if (ctx.get<HitCounter>().hits != 1) return 6;
        if (!check("method-level", {"tag:Api.tagged"})) return 7;
    }

    // ── 3. 多个切面：标注顺序即层次（tag 外、deny 内），deny 短路 ──────
    g_log.clear();
    {
        const Response resp = r.dispatch("GET", "/denied", "");
        if (resp.status != 403 || std::string_view(resp.body) != "denied") return 8;
        if (!has_header(resp, "X-Tag")) return 9;       // 外层 tag 仍执行到 after
        if (ctx.get<HitCounter>().hits != 2) return 10;
        if (!check("short-circuit", {"tag:Api.denied", "deny"})) return 11;
    }

    // ── 4. 重复标注只生效一次 ──────────────────────────────────────────
    g_log.clear();
    {
        if (r.dispatch("GET", "/dup", "").status != 200) return 12;
        if (ctx.get<HitCounter>().hits != 3) return 13;
        if (!check("dedup", {"tag:Api.dup"})) return 14;
    }

    // ── 5. 控制器级 + 方法级追加；全局切面在最外层且全 bean 共享一份 ──────
    g_log.clear();
    {
        const Response resp = r.dispatch("GET", "/admin", "");
        if (resp.status != 200 || !has_header(resp, "X-Tag")) return 15;
        if (!check("class-level",
                   {"enter:AdminApi.admin#1", "tag:AdminApi.admin"}))
            return 16;
    }
    g_log.clear();
    {
        const Response resp = r.dispatch("GET", "/admin/all", "");
        if (resp.status != 403) return 17;
        if (!has_header(resp, "X-Tag")) return 18;
        // #2：用了 UseAspect 的 bean，全局切面仍是共享链而非每路由一份副本
        if (!check("class+method",
                   {"enter:AdminApi.all#2", "tag:AdminApi.all", "deny"}))
            return 19;
    }

    // ── 6. 切面 Bean 在 profile 下未激活 → 启动期报错（不静默跳过）──────
    {
        router r2;
        bool threw = false;
        try {
            mount_routes(r2, ctx, ctx.get<ProdApi>());
        } catch (const std::exception& e) {
            threw = std::string_view(e.what()).find("UseAspect")
                    != std::string_view::npos;
        }
        if (!threw) return 20;
    }

    // argc>200 才运行：实例化 application 引导（自动发现 → 容器 → 注册表挂载）。
    if (argc > 200) return AspectApp{}.run();

    std::println("METHOD ASPECT SMOKE: PASS");
    return 0;
}
