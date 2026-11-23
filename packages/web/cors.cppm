/**
 * @file cors.cppm
 * @brief CORS 中间件 — 来源白名单、预检短路、凭证与暴露头
 *
 * 作为契约组件 `cors_component` 使用，全局 middleware 顺序 -1000，
 * 在 AuthGuard（order 100）之前处理 OPTIONS 预检。
 *
 * 行为：
 *   - 无 Origin → 直接放行（非跨域请求）
 *   - Origin 不匹配白名单 → 不加 CORS 头，交给后续链
 *   - 预检（OPTIONS + Access-Control-Request-Method）→ 204 短路
 *   - 实际请求 → 执行后续链并附加 CORS 头
 *
 * 白名单支持 `*` 通配：`*`、`https://*.example.com`。
 * allow_credentials=true 时禁止裸 `*`（浏览器不接受该组合）。
 */
export module cboot.web.cors;

import std;
import cboot.core.annotations;   // Bean / Config / bean_name
import cboot.meta.reflect;       // type_list
import cboot.web.http;           // Request / Response
import cboot.web.middleware;     // Middleware / request_context / next_fn

export namespace web {

/** @brief CORS 配置（前缀 cors） */
struct [[=Config{"cors"}]] CorsProps {
    bool enabled{true};
    std::vector<std::string> allowed_origins{"*"};
    bool allow_credentials{false};
    std::vector<std::string> allowed_methods{
        "GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    std::vector<std::string> allowed_headers{"Content-Type", "Authorization"};
    std::vector<std::string> exposed_headers;
    int max_age_s{600};
};

namespace detail {

bool cors_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

std::string_view cors_trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

/** @brief 通配匹配：`*` 匹配任意字符序列 */
bool cors_glob(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string join_values(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& v : values) {
        if (!out.empty()) out += ", ";
        out += v;
    }
    return out;
}

bool contains_ci(const std::vector<std::string>& values, std::string_view v) {
    for (const auto& item : values)
        if (cors_iequals(item, v)) return true;
    return false;
}

} // namespace detail

/**
 * @brief 全局 CORS 中间件
 *
 * 构造时校验配置；请求期只读。预检请求不进入路由与 AuthGuard。
 */
class [[=Bean{}]] [[=Middleware{.order = -1000}]] Cors {
public:
    explicit Cors(CorsProps& props) : props_(props) {
        if (props_.allow_credentials
            && detail::contains_ci(props_.allowed_origins, "*"))
            throw std::invalid_argument(
                "cboot.web.cors: allow_credentials requires explicit allowed_origins");
        if (props_.max_age_s < 0)
            throw std::invalid_argument(
                "cboot.web.cors: max_age_s must not be negative");
    }

    Response handle(request_context& ctx, const next_fn& next) {
        if (!props_.enabled) return next();

        const auto origin = ctx.request.header("Origin");
        if (!origin || origin->empty()) return next();
        if (!origin_allowed(*origin)) return next();

        const auto requested_method =
            ctx.request.header("Access-Control-Request-Method");
        if (ctx.request.method == "OPTIONS" && requested_method) {
            if (!method_allowed(*requested_method)
                || !headers_allowed(
                    ctx.request.header("Access-Control-Request-Headers")
                        .value_or("")))
                return rejected();

            Response r;
            r.status = 204;
            apply_origin(r, *origin, /*preflight=*/true);
            r.header("Access-Control-Allow-Methods",
                     detail::join_values(props_.allowed_methods));

            std::string headers = allow_headers_value(
                ctx.request.header("Access-Control-Request-Headers"));
            if (!headers.empty())
                r.header("Access-Control-Allow-Headers", std::move(headers));
            if (props_.max_age_s > 0)
                r.header("Access-Control-Max-Age",
                         std::to_string(props_.max_age_s));
            return r;
        }

        Response r = next();
        apply_origin(r, *origin, /*preflight=*/false);
        if (!props_.exposed_headers.empty())
            r.header("Access-Control-Expose-Headers",
                     detail::join_values(props_.exposed_headers));
        return r;
    }

private:
    bool origin_allowed(std::string_view origin) const {
        for (const auto& pattern : props_.allowed_origins)
            if (detail::cors_glob(pattern, origin)) return true;
        return false;
    }

    bool literal_wildcard() const {
        for (const auto& pattern : props_.allowed_origins)
            if (pattern == "*") return true;
        return false;
    }

    bool method_allowed(std::string_view method) const {
        for (const auto& item : props_.allowed_methods)
            if (item == "*" || detail::cors_iequals(item, method)) return true;
        return false;
    }

    bool headers_allowed(std::string_view requested) const {
        if (requested.empty()) return true;
        if (detail::contains_ci(props_.allowed_headers, "*")) return true;

        std::string_view rest = requested;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view item =
                detail::cors_trim(rest.substr(0, comma));
            if (!item.empty()
                && !detail::contains_ci(props_.allowed_headers, item))
                return false;
            rest = comma == std::string_view::npos
                       ? std::string_view{}
                       : rest.substr(comma + 1);
        }
        return true;
    }

    std::string allow_headers_value(
        std::optional<std::string_view> requested) const {
        if (detail::contains_ci(props_.allowed_headers, "*")) {
            if (requested) return std::string(*requested);
            return {};
        }
        return detail::join_values(props_.allowed_headers);
    }

    void apply_origin(Response& r, std::string_view origin,
                      bool preflight) const {
        if (props_.allow_credentials || !literal_wildcard())
            r.header("Access-Control-Allow-Origin", std::string(origin));
        else
            r.header("Access-Control-Allow-Origin", "*");

        if (props_.allow_credentials)
            r.header("Access-Control-Allow-Credentials", "true");

        r.header("Vary", preflight
            ? "Origin, Access-Control-Request-Method, Access-Control-Request-Headers"
            : "Origin");
    }

    static Response rejected() {
        Response r = Response::text("cors rejected");
        r.status = 403;
        return r;
    }

    CorsProps& props_;
};

} // namespace web

/**
 * @brief CORS 契约组件
 *
 * 应用显式加入 components；不加入则无任何 CORS 行为。
 */
export struct cors_component {
    static constexpr bean_name name{"web.cors"};
    using beans = type_list<web::CorsProps, web::Cors>;
    using dependencies = type_list<>;
};
