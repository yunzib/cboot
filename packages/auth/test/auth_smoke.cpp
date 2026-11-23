/**
 * @file auth_smoke.cpp
 * @brief 认证组件冒烟测试
 *
 * 覆盖：
 *   - AuthService login/find/logout/kickout
 *   - AuthGuard 路由策略（public / protected / default）
 *   - 401、auth::session 注入、认证事件
 *   - auth_component 依赖闭包
 */
import cboot.app;
import cboot.auth;
import cboot.json;
import cboot.web.http;
import cboot.web.middleware;
import std;

struct AuthApp : application<AuthApp> {
    using components = type_list<auth_component>;
};

template <class L, class T>
struct contains;
template <class... Ts, class T>
struct contains<type_list<Ts...>, T>
    : std::bool_constant<(std::is_same_v<Ts, T> || ...)> {};

static_assert(contains<app_beans_t<AuthApp>, auth::AuthProps>::value);
static_assert(contains<app_beans_t<AuthApp>, auth::MemoryTokenStore>::value);
static_assert(contains<app_beans_t<AuthApp>, auth::AuthService>::value);
static_assert(contains<app_beans_t<AuthApp>, auth::AuthGuard>::value);

struct Api {
    [[=Post{"/api/auth/login"}]]
    Response login() {
        return json_response(json::value("login"));
    }

    [[=Get{"/api/me"}]]
    Response me(const web::request_context& ctx) {
        const auto* s = ctx.find<auth::session>();
        return json_response(json::value(s ? s->subject : "none"));
    }

    [[=Get{"/api/public"}]]
    Response public_route() {
        return json_response(json::value("public"));
    }

    [[=Get{"/health"}]]
    Response health() {
        return json_response(json::value("up"));
    }
};

int main(int argc, char**) {
    auth::AuthProps props;
    props.timeout_s = 3600;
    props.default_policy = "protected";
    props.public_paths = {
        "POST:/api/auth/login",
        "GET:/api/public",
        "GET:/health",
    };
    props.protected_paths = {"/api/**"};

    auth::MemoryTokenStore store(props);
    auth::AuthService service(store, props);

    // ── TokenStore / AuthService ───────────────────────────────────────
    const std::string token = service.login(auth::session{
        .subject = "user-42",
        .roles = {"admin"},
        .scopes = {"read"},
    });
    if (token.empty()) return 1;

    auto found = service.find(token);
    if (!found || found->subject != "user-42") return 2;

    // 过期 token
    const auto expired = service.login(auth::session{
        .subject = "expired",
        .expires_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 1,
    });
    if (service.find(expired).has_value()) return 3;

    // kickout
    service.kickout("user-42");
    if (service.find(token).has_value()) return 4;

    // ── AuthGuard + 路由策略 + 事件 ────────────────────────────────────
    struct Counts { int ok = 0; int failed = 0; };
    Counts counts;
    event_publisher::dispatch_map map;
    map[std::type_index(typeid(auth::authentication_succeeded))] =
        [](void* self, const void*) -> event_result {
            ++static_cast<Counts*>(self)->ok;
            return {.delivered = 1};
        };
    map[std::type_index(typeid(auth::authentication_failed))] =
        [](void* self, const void*) -> event_result {
            ++static_cast<Counts*>(self)->failed;
            return {.delivered = 1};
        };
    event_publisher bus;
    bus.bind(&counts, &map);

    auth::AuthGuard guard(service, props, bus);
    Api api;
    router r;
    r.mount(api);
    r.use("", 100, [&guard](web::request_context& ctx, const web::next_fn& next) {
        return guard.handle(ctx, next);
    });

    // public 路由放行
    if (r.dispatch("POST", "/api/auth/login", "").status != 200) return 5;
    if (r.dispatch("GET", "/api/public", "").status != 200) return 6;
    if (r.dispatch("GET", "/health", "").status != 200) return 7;

    // protected 无 token → 401
    counts = {};
    auto denied = r.dispatch("GET", "/api/me", "");
    if (denied.status != 401) return 8;
    if (counts.failed != 1 || counts.ok != 0) return 9;

    // 登录成功 → principal + success 事件
    const std::string fresh = service.login(auth::session{.subject = "user-7"});
    const std::string headers = "Authorization: Bearer " + fresh + "\r\n";
    counts = {};
    auto ok = r.dispatch("GET", "/api/me", "", headers);
    if (ok.status != 200 || json::parse(ok.body).as_string() != "user-7")
        return 10;
    if (counts.ok != 1 || counts.failed != 0) return 11;

    // logout 后 token 失效
    service.logout(fresh);
    if (r.dispatch("GET", "/api/me", "", headers).status != 401) return 12;

    // 未知路径不触发认证（404）
    counts = {};
    if (r.dispatch("GET", "/missing", "").status != 404) return 13;
    if (counts.failed != 0) return 14;

    // 组件 bean 通过真实 context 组装（TokenStore 基类注入 / event_publisher）
    {
        using AuthCtx = context_for<app_beans_t<AuthApp>>;
        auto cfg = config::properties::from_json(R"({"auth":{}})");
        AuthCtx ctx(&cfg);
        auto& app_service = ctx.get<auth::AuthService>();
        const std::string t = app_service.login(auth::session{.subject = "ctx-user"});
        if (!app_service.find(t)) return 15;
    }

    // argc>200 才运行：实例化 application::run / middleware 注册路径。
    if (argc > 200) return AuthApp{}.run();

    std::println("AUTH SMOKE: PASS");
    return 0;
}
