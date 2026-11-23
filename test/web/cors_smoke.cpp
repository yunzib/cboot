/**
 * @file cors_smoke.cpp
 * @brief CORS 中间件冒烟测试
 *
 * 覆盖：
 *   - 实际请求附加 CORS 头与暴露头
 *   - 来源通配（子域）与拒绝不一致来源
 *   - 预检短路 204、方法/请求头校验
 *   - 默认 `*` 与 credentials 冲突校验
 *   - enabled=false 直通
 */
import cboot.app;
import cboot.web.cors;
import cboot.web.http;
import cboot.web.middleware;
import std;

template <class L, class T>
struct contains;
template <class... Ts, class T>
struct contains<type_list<Ts...>, T>
    : std::bool_constant<(std::is_same_v<Ts, T> || ...)> {};

struct CorsApp : application<CorsApp> {
    using components = type_list<cors_component>;
};

static_assert(contains<app_beans_t<CorsApp>, web::CorsProps>::value);
static_assert(contains<app_beans_t<CorsApp>, web::Cors>::value);

namespace {

bool has_header(const Response& r, std::string_view name,
                std::string_view value) {
    for (const auto& [k, v] : r.headers)
        if (k == name && v == value) return true;
    return false;
}

Response ok_next() {
    Response r;
    r.body = std::string("ok");
    return r;
}

web::next_fn counting_next(int& calls) {
    return [&calls] {
        ++calls;
        return ok_next();
    };
}

} // namespace

int main() {
    web::CorsProps props;
    props.allowed_origins = {"https://app.example.com", "https://*.trusted.dev"};
    props.allow_credentials = true;
    props.exposed_headers = {"X-Request-Id"};

    web::Cors cors(props);

    // ── 实际请求：允许来源 ─────────────────────────────────────────────
    {
        const std::string headers = "Origin: https://app.example.com\r\n";
        const Request req{.method = "GET", .path = "/api/users",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = cors.handle(ctx, counting_next(calls));
        if (calls != 1 || r.status != 200) return 1;
        if (!has_header(r, "Access-Control-Allow-Origin",
                        "https://app.example.com"))
            return 2;
        if (!has_header(r, "Access-Control-Allow-Credentials", "true")) return 3;
        if (!has_header(r, "Access-Control-Expose-Headers", "X-Request-Id"))
            return 4;
        if (!has_header(r, "Vary", "Origin")) return 5;
    }

    // ── 来源通配：子域匹配 ─────────────────────────────────────────────
    {
        const std::string headers = "Origin: https://a.trusted.dev\r\n";
        const Request req{.method = "GET", .path = "/api/users",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = cors.handle(ctx, counting_next(calls));
        if (calls != 1) return 6;
        if (!has_header(r, "Access-Control-Allow-Origin",
                        "https://a.trusted.dev"))
            return 7;
    }

    // ── 未匹配来源：不加 CORS 头，仍放行 ───────────────────────────────
    {
        const std::string headers = "Origin: https://evil.example\r\n";
        const Request req{.method = "GET", .path = "/api/users",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = cors.handle(ctx, counting_next(calls));
        if (calls != 1) return 8;
        for (const auto& [k, v] : r.headers)
            if (k == "Access-Control-Allow-Origin") return 9;
    }

    // ── 预检：允许的方法与请求头 ───────────────────────────────────────
    {
        const std::string headers =
            "Origin: https://app.example.com\r\n"
            "Access-Control-Request-Method: POST\r\n"
            "Access-Control-Request-Headers: Content-Type, Authorization\r\n";
        const Request req{.method = "OPTIONS", .path = "/api/users",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = cors.handle(ctx, counting_next(calls));
        if (calls != 0 || r.status != 204) return 10;
        if (!has_header(r, "Access-Control-Allow-Origin",
                        "https://app.example.com"))
            return 11;
        if (!has_header(r, "Access-Control-Allow-Methods",
                        "GET, POST, PUT, PATCH, DELETE, OPTIONS"))
            return 12;
        if (!has_header(r, "Access-Control-Allow-Headers",
                        "Content-Type, Authorization"))
            return 13;
        if (!has_header(r, "Access-Control-Max-Age", "600")) return 14;
        if (!has_header(r, "Vary",
                        "Origin, Access-Control-Request-Method, "
                        "Access-Control-Request-Headers"))
            return 15;
    }

    // ── 预检：方法不在白名单 → 403 ─────────────────────────────────────
    {
        const std::string headers =
            "Origin: https://app.example.com\r\n"
            "Access-Control-Request-Method: TRACE\r\n";
        const Request req{.method = "OPTIONS", .path = "/api/users",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = cors.handle(ctx, counting_next(calls));
        if (calls != 0 || r.status != 403) return 16;
    }

    // ── 预检：请求头不在白名单 → 403 ───────────────────────────────────
    {
        const std::string headers =
            "Origin: https://app.example.com\r\n"
            "Access-Control-Request-Method: GET\r\n"
            "Access-Control-Request-Headers: X-Secret\r\n";
        const Request req{.method = "OPTIONS", .path = "/api/users",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = cors.handle(ctx, counting_next(calls));
        if (calls != 0 || r.status != 403) return 17;
    }

    // ── 默认 `*`（无 credentials）→ 输出 `*` ───────────────────────────
    {
        web::CorsProps wild;
        web::Cors wild_cors(wild);
        const std::string headers = "Origin: https://anywhere.example\r\n";
        const Request req{.method = "GET", .path = "/x",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = wild_cors.handle(ctx, counting_next(calls));
        if (calls != 1
            || !has_header(r, "Access-Control-Allow-Origin", "*"))
            return 18;
    }

    // ── credentials + `*` → 构造报错 ───────────────────────────────────
    {
        web::CorsProps bad;
        bad.allow_credentials = true;
        bool threw = false;
        try {
            web::Cors bad_cors(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) return 19;
    }

    // ── enabled=false → 完全直通 ───────────────────────────────────────
    {
        web::CorsProps off;
        off.enabled = false;
        web::Cors off_cors(off);
        const std::string headers = "Origin: https://anywhere.example\r\n";
        const Request req{.method = "GET", .path = "/x",
                               .query = {}, .body = {}, .headers = headers};
        web::request_context ctx(req);
        int calls = 0;
        const Response r = off_cors.handle(ctx, counting_next(calls));
        if (calls != 1 || !r.headers.empty()) return 20;
    }

    std::println("CORS SMOKE: PASS");
    return 0;
}
