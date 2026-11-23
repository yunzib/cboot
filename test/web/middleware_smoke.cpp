/**
 * @file middleware_smoke.cpp
 * @brief middleware 冒烟测试（纯逻辑，无 socket）
 *
 * 覆盖：
 *   - 全局 middleware / 路由级 UseMiddleware 标签
 *   - 顺序、短路、request_context、Request headers
 *   - OPTIONS preflight 由全局 middleware 处理
 */
import cboot.json;
import cboot.web.http;
import cboot.web.middleware;
import cboot.web.routing;
import std;

struct Api {
    [[=Get{"/public"}]]
    Response public_route(const Request& req,
                          const web::request_context& ctx) {
        auto* n = ctx.find<int>();
        json::object out;
        out.emplace_back("header",
                         json::value(std::string(req.header("X-Test").value_or("none"))));
        out.emplace_back("context", json::value(n ? *n : -1));
        return json_response(json::value(std::move(out)));
    }

    [[=Get{"/private"}]]
    [[=web::UseMiddleware{"auth"}]]
    Response private_route(const web::request_context& ctx) {
        auto* n = ctx.find<int>();
        return json_response(json::value(n ? *n : -1));
    }
};

int main() {
    Api api;
    router r;
    r.mount(api);

    // 全局：注入 int + 处理 OPTIONS preflight
    r.use("", 0, [](web::request_context& ctx, const web::next_fn& next) {
        if (ctx.request.method == "OPTIONS") {
            Response resp;
            resp.status = 204;
            resp.header("Access-Control-Allow-Origin", "*");
            return resp;
        }
        ctx.put(7);
        Response resp = next();
        resp.header("X-Trace", "global");
        return resp;
    });

    // 路由级：只对 [[=UseMiddleware{"auth"}]] 的路由生效
    r.use("auth", 100, [](web::request_context& ctx, const web::next_fn& next) {
        if (ctx.request.header("X-Deny")) {
            Response resp = Response::text("denied");
            resp.status = 401;
            return resp;
        }
        ctx.put(42);
        return next();
    });

    if (r.middleware_count() != 2) return 1;

    // 全局 + headers + request_context
    auto pub = r.dispatch("GET", "/public", "", "X-Test: hello\r\n");
    if (pub.status != 200) return 2;
    auto pv = json::parse(pub.body);
    if (pv.find("header")->as_string() != "hello") return 3;
    if (pv.find("context")->as_int() != 7) return 4;
    bool trace = false;
    for (auto& [k, v] : pub.headers)
        if (k == "X-Trace" && v == "global") trace = true;
    if (!trace) return 5;

    // 路由级：无 deny → 覆盖 int 并放行
    auto priv = r.dispatch("GET", "/private", "");
    if (priv.status != 200 || json::parse(priv.body).as_int() != 42) return 6;

    // 路由级：deny → 401 短路
    auto denied = r.dispatch("GET", "/private", "", "X-Deny: 1\r\n");
    if (denied.status != 401) return 7;

    // 未命中 404 仍经过全局 middleware
    auto missing = r.dispatch("GET", "/missing", "");
    if (missing.status != 404) return 8;
    trace = false;
    for (auto& [k, v] : missing.headers)
        if (k == "X-Trace" && v == "global") trace = true;
    if (!trace) return 9;

    // OPTIONS preflight：无路由，全局 middleware 直接 204
    auto preflight = r.dispatch("OPTIONS", "/private", "");
    if (preflight.status != 204) return 10;
    bool cors = false;
    for (auto& [k, v] : preflight.headers)
        if (k == "Access-Control-Allow-Origin" && v == "*") cors = true;
    if (!cors) return 11;

    std::println("WEB MIDDLEWARE SMOKE: PASS");
    return 0;
}
