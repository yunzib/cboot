/**
 * @file error_smoke.cpp
 * @brief 全局错误层冒烟测试
 *
 * 覆盖：
 *   - 默认 404/405/401/校验 400 响应
 *   - ErrorHandler 捕获 web::error / validation / std::exception
 *   - ErrorMapper 顺序、nullopt 回退、协议头合并
 *   - router 标记错误 + ErrorHandler 接管
 *   - request_context::get<T>() 缺失抛错
 */
import cboot.core.annotations;  // Bean
import cboot.validation;
import cboot.web.error;
import cboot.web.http;
import cboot.web.middleware;
import cboot.web.routing;
import std;

namespace {

struct Missing {};

struct Dto {
    [[=validation::NotBlank{"name required"}]] std::string name;
};

struct [[=Bean{}]] Api {
    [[=Get{"/ok"}]]
    Response ok() { return Response::text("ok"); }

    [[=Get{"/throw"}]]
    Response boom() {
        web::error_info info;
        info.status = 418;
        info.code = "teapot";
        info.message = "teapot";
        throw web::error(std::move(info));
    }

    [[=Get{"/auth"}]]
    Response auth_fail() {
        web::error_info info;
        info.status = 401;
        info.code = "auth.unauthorized";
        info.message = "unauthorized";
        info.headers.emplace_back("WWW-Authenticate", "Bearer");
        throw web::error(std::move(info));
    }

    [[=Get{"/std"}]]
    Response std_boom() { throw std::runtime_error("boom"); }

    [[=Post{"/dto"}]]
    Response dto(const Dto&) { return Response{}; }
};

bool has_header(const Response& r, std::string_view name,
                std::string_view value) {
    for (const auto& [k, v] : r.headers)
        if (k == name && v == value) return true;
    return false;
}

bool has_header(const Response& r, std::string_view name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return true;
    return false;
}

} // namespace

int main() {
    // ── 默认响应 ───────────────────────────────────────────────────────
    {
        web::error_info info;
        info.status = 404;
        info.code = "not_found";
        info.message = "not found";
        const Response r = web::default_error_response(info);
        if (r.status != 404 || std::string_view(r.body) != "404 not found")
            return 1;
    }
    {
        web::error_info info;
        info.status = 405;
        info.code = "method_not_allowed";
        info.message = "method not allowed";
        info.headers.emplace_back("Allow", "POST");
        const Response r = web::default_error_response(info);
        if (r.status != 405 || std::string_view(r.body) != "405 method not allowed")
            return 2;
        if (!has_header(r, "Allow", "POST")) return 3;
    }
    {
        web::error_info info;
        info.status = 401;
        info.code = "auth.unauthorized";
        info.headers.emplace_back("WWW-Authenticate", "Bearer");
        const Response r = web::default_error_response(info);
        if (r.status != 401 || !has_header(r, "WWW-Authenticate", "Bearer"))
            return 4;
    }

    // ── registry：顺序、nullopt 回退、头合并 ──────────────────────────
    web::error_registry registry;
    registry.add(20, [](const web::error_info& info,
                        const web::request_context&) -> std::optional<Response> {
        if (info.status != 405) return std::nullopt;
        Response r = Response::text("custom-405");
        r.status = 405;
        return r;       // 不含 Allow，registry 应补上
    });
    registry.add(10, [](const web::error_info& info,
                        const web::request_context&) -> std::optional<Response> {
        if (info.status != 418) return std::nullopt;
        Response r = Response::text("custom-teapot");
        r.status = 418;
        return r;
    });
    registry.add(30, [](const web::error_info& info,
                        const web::request_context&) -> std::optional<Response> {
        if (info.status != 401) return std::nullopt;
        Response r = Response::text("custom-401");
        r.status = 401;
        return r;
    });

    web::ErrorHandler handler(registry);
    router r;
    Api api;
    r.mount(api);
    r.use("", web::error_middleware_order,
          [&handler](web::request_context& ctx, const web::next_fn& next) {
              return handler.handle(ctx, next);
          });

    // 正常请求不经过 mapper
    {
        const Response response = r.dispatch("GET", "/ok", "");
        if (response.status != 200 || std::string_view(response.body) != "ok")
            return 5;
    }

    // web::error → 自定义 418
    {
        const Response response = r.dispatch("GET", "/throw", "");
        if (response.status != 418
            || std::string_view(response.body) != "custom-teapot")
            return 6;
    }

    // 401 + WWW-Authenticate 合并
    {
        const Response response = r.dispatch("GET", "/auth", "");
        if (response.status != 401
            || std::string_view(response.body) != "custom-401")
            return 7;
        if (!has_header(response, "WWW-Authenticate", "Bearer")) return 8;
    }

    // std::exception → 默认 500
    {
        const Response response = r.dispatch("GET", "/std", "");
        if (response.status != 500
            || std::string_view(response.body) != "500 boom")
            return 9;
    }

    // 404 默认
    {
        const Response response = r.dispatch("GET", "/missing", "");
        if (response.status != 404
            || std::string_view(response.body) != "404 not found")
            return 10;
    }

    // 405 + 自定义 body + 补 Allow
    {
        const Response response = r.dispatch("GET", "/dto", "");
        if (response.status != 405
            || std::string_view(response.body) != "custom-405")
            return 11;
        if (!has_header(response, "Allow", "POST")) return 12;
    }

    // 校验失败 → 400 + errors 字段
    {
        const Response response = r.dispatch("POST", "/dto", R"({"name":"  "})");
        if (response.status != 400) return 13;
        const json::value body = json::parse(response.body);
        const auto* errors = body.find("errors");
        if (!errors || errors->as_array().empty()) return 14;
    }

    // 直接 router（无 ErrorHandler）仍保持默认错误响应
    {
        router plain;
        Api plain_api;
        plain.mount(plain_api);
        const Response response = plain.dispatch("GET", "/throw", "");
        if (response.status != 418
            || std::string_view(response.body) != "418 teapot")
            return 15;
    }

    // ErrorHandler 直接捕获下游异常（不经 router）
    {
        Request req{};
        web::request_context ctx(req);
        web::next_fn boom = []() -> Response {
            web::error_info info;
            info.status = 418;
            info.code = "teapot";
            info.message = "teapot";
            throw web::error(std::move(info));
        };
        const Response response = handler.handle(ctx, boom);
        if (response.status != 418
            || std::string_view(response.body) != "custom-teapot")
            return 16;
    }

    // get<T>() 缺失 → logic_error
    {
        Request req{};
        web::request_context ctx(req);
        bool threw = false;
        try {
            (void)ctx.get<Missing>();
        } catch (const std::logic_error&) {
            threw = true;
        }
        if (!threw) return 17;
    }

    std::println("ERROR SMOKE: PASS");
    return 0;
}
