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
    using components = type_list<auth_memory_component>;
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
    [[=auth::RequireRole{"admin"}]]
    [[=auth::RequireScope{"write"}]]
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

struct [[=Bean{}]] [[=auth::RequireRole{"admin"}]] RoleApi {
    [[=Get{"/admin"}]]
    [[=auth::RequireScope{"write"}]]
    Response admin() {
        return json_response(json::value("admin"));
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

    auth::AuthPolicy policy;
    policy.set("Api.me", auth::route_policy{
        .roles = {"admin"}, .scopes = {"write"}});
    auth::AuthGuard guard(service, props, policy, bus);
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
    const std::string fresh = service.login(auth::session{
        .subject = "user-7", .roles = {"admin"}, .scopes = {"write"}});
    const std::string headers = "Authorization: Bearer " + fresh + "\r\n";
    counts = {};
    auto ok = r.dispatch("GET", "/api/me", "", headers);
    if (ok.status != 200 || json::parse(ok.body).as_string() != "user-7")
        return 10;
    if (counts.ok != 1 || counts.failed != 0) return 11;

    // 已登录但缺少角色/scope → 403
    const std::string limited = service.login(auth::session{.subject = "user-8"});
    const std::string limited_headers =
        "Authorization: Bearer " + limited + "\r\n";
    if (r.dispatch("GET", "/api/me", "", limited_headers).status != 403)
        return 15;

    // logout 后 token 失效
    service.logout(fresh);
    if (r.dispatch("GET", "/api/me", "", headers).status != 401) return 12;

    // ── Cookie 传输 / 滑动续期 ─────────────────────────────────────────
    props.token_cookie = "cboot_token";
    const std::string cookie_token = service.login(auth::session{
        .subject = "cookie-user", .roles = {"admin"}, .scopes = {"write"}});

    const std::string cookie_headers =
        "Cookie: theme=dark; cboot_token=" + cookie_token + "\r\n";
    const Request cookie_req{.method = "GET", .path = "/api/me",
                                  .query = {}, .body = {},
                                  .headers = cookie_headers};
    web::request_context cookie_ctx(cookie_req);
    const auto cookie_session = service.current(cookie_ctx);
    if (!cookie_session || cookie_session->subject != "cookie-user") return 18;
    if (service.token(cookie_ctx) != cookie_token) return 19;

    // header 优先于 Cookie
    const std::string header_token = service.login(
        auth::session{.subject = "header-user", .roles = {"admin"}});
    const std::string both_headers = "Authorization: Bearer " + header_token
        + "\r\nCookie: cboot_token=" + cookie_token + "\r\n";
    const Request both_req{.method = "GET", .path = "/api/me",
                                .query = {}, .body = {}, .headers = both_headers};
    web::request_context both_ctx(both_req);
    const auto both_session = service.current(both_ctx);
    if (!both_session || both_session->subject != "header-user") return 20;

    // Set-Cookie 辅助
    Response set_cookie;
    service.attach_cookie(set_cookie, cookie_token);
    bool cookie_ok = false;
    for (const auto& [name, value] : set_cookie.headers)
        if (name == "Set-Cookie")
            cookie_ok = value.find("cboot_token=" + cookie_token)
                    != std::string::npos
                && value.find("HttpOnly") != std::string::npos
                && value.find("SameSite=Lax") != std::string::npos;
    if (!cookie_ok) return 21;

    Response clear_cookie;
    service.clear_cookie(clear_cookie);
    bool cleared = false;
    for (const auto& [name, value] : clear_cookie.headers)
        if (name == "Set-Cookie")
            cleared = value.find("cboot_token=;") != std::string::npos
                && value.find("Max-Age=0") != std::string::npos;
    if (!cleared) return 22;

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    props.active_timeout_s = 60;
    const std::string sliding = service.login(auth::session{
        .subject = "slide-user",
        .issued_at = now - 100,
        .expires_at = now + 10,
    });
    const auto before = service.find(sliding)->expires_at;
    const std::string slide_headers =
        "Authorization: Bearer " + sliding + "\r\n";
    const Request slide_req{.method = "GET", .path = "/api/me",
                                 .query = {}, .body = {},
                                 .headers = slide_headers};
    web::request_context slide_ctx(slide_req);
    if (!service.current(slide_ctx)) return 23;
    const auto after = service.find(sliding)->expires_at;
    if (after <= before || after > now + 61) return 24;

    // 滑动不能突破 issued_at + timeout_s 的绝对上限
    props.active_timeout_s = 600;
    const std::string capped = service.login(auth::session{
        .subject = "cap-user",
        .issued_at = now - 3590,
        .expires_at = now + 5,
    });
    const std::string cap_headers =
        "Authorization: Bearer " + capped + "\r\n";
    const Request cap_req{.method = "GET", .path = "/api/me",
                          .query = {}, .body = {}, .headers = cap_headers};
    web::request_context cap_ctx(cap_req);
    if (!service.current(cap_ctx)) return 25;
    if (service.find(capped)->expires_at > now + 11) return 26;
    props.active_timeout_s = 0;

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
        if (!app_service.find(t)) return 16;
    }

    // 启动期 hook 扫描 Bean 方法上的 RequireRole/RequireScope
    {
        using HookCtx = context<auth::AuthPolicy, RoleApi>;
        HookCtx hook_ctx;
        auth::auth_policy_hook::instance<HookCtx> hook(hook_ctx);
        const auto* p = hook_ctx.get<auth::AuthPolicy>().find("RoleApi.admin");
        if (!p || p->roles.size() != 1 || p->roles[0] != "admin"
            || p->scopes.size() != 1 || p->scopes[0] != "write")
            return 17;
    }

    // argc>200 才运行：实例化 application::run / middleware 注册路径。
    if (argc > 200) return AuthApp{}.run();

    std::println("AUTH SMOKE: PASS");
    return 0;
}
