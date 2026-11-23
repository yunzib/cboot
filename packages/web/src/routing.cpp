/**
 * @file routing.cpp
 * @brief cboot.web.routing 实现单元 — 路由匹配、middleware 链与请求错误映射
 *
 * 负责 URL 解码、路径匹配、405 Allow、全局/路由级中间件执行，
 * 以及 body DTO 校验错误到 400 的映射。
 */
module cboot.web.routing;

import std;
import cboot.validation;
import cboot.web.http;
import cboot.web.middleware;
import cboot.web.error;

namespace {

// 路径切段（'/' 分隔，忽略空段）+ 逐段百分号解码
// 零分配快路径：路径无 '%' 时全部为指向原串的视图；含 '%' 时解码进 scratch
// （先 reserve，视图指向 scratch 不会因扩容失效）
struct segments {
    std::array<std::string_view, 16> items{};
    std::size_t count = 0;
};

segments split_view(std::string_view path, std::string& scratch) {
    segments out;
    const bool need_decode = path.find('%') != std::string_view::npos;
    if (need_decode) scratch.reserve(scratch.size() + path.size() + 1);
    std::size_t i = 0;
    while (i < path.size() && out.count < out.items.size()) {
        std::size_t slash = path.find('/', i);
        if (slash == std::string_view::npos) slash = path.size();
        std::string_view seg = path.substr(i, slash - i);
        if (!seg.empty()) {
            if (!need_decode) {
                out.items[out.count++] = seg;
            } else {
                const std::size_t before = scratch.size();
                url_decode_append(scratch, seg);
                out.items[out.count++] =
                    std::string_view(scratch.data() + before, scratch.size() - before);
            }
        }
        i = slash + 1;
    }
    return out;
}

std::string join_segments(const segments& segs) {
    std::string out;
    for (std::size_t i = 0; i < segs.count; ++i) {
        out.push_back('/');
        out.append(segs.items[i]);
    }
    return out;
}

/** @brief 路径匹配（忽略方法）；命中时填充参数段视图 */
bool path_matches(const std::vector<std::string>& pattern,
                  const segments& target,
                  std::vector<std::string_view>& pp) {
    if (pattern.size() != target.count) return false;
    std::vector<std::string_view> tmp;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (!pattern[i].empty() && pattern[i][0] == '{') {
            tmp.push_back(target.items[i]);        // 参数段：按位收集
        } else if (pattern[i] != target.items[i]) {
            return false;
        }
    }
    pp = std::move(tmp);
    return true;
}

} // namespace

void router::add_route(http_method method, std::string_view pattern,
                       detail::handler_fn h, std::string bean,
                       std::string handler,
                       std::vector<std::string> middleware) {
    route_item item{method, std::string(pattern), {}, std::move(h),
                    std::move(bean), std::move(handler), std::move(middleware)};
    {
        std::string scratch;
        const segments segs = split_view(pattern, scratch);
        for (std::size_t i = 0; i < segs.count; ++i)
            item.segs.emplace_back(segs.items[i]);
    }
    routes_.push_back(std::move(item));
}

void router::use(std::string name, int order, web::middleware_fn fn) {
    if (!fn) throw std::invalid_argument("cboot.web: empty middleware");
    middlewares_.push_back({std::move(name), order, std::move(fn)});
    std::stable_sort(middlewares_.begin(), middlewares_.end(),
        [](const middleware_entry& a, const middleware_entry& b) {
            if (a.order != b.order) return a.order < b.order;
            return a.name < b.name;
        });
}

Response router::dispatch(std::string_view method, std::string_view target,
                          std::string_view body) const {
    return dispatch(method, target, body, {});
}

Response router::dispatch(std::string_view method, std::string_view target,
                          std::string_view body, std::string_view headers) const {
    // 方法解析（未知方法 → 405 并给 Allow）
    const std::optional<http_method> m = method_from_name(method);
    std::string_view path = target;
    std::string_view query;
    if (std::size_t q = target.find('?'); q != std::string_view::npos) {
        path = target.substr(0, q);
        query = target.substr(q + 1);
    }
    std::string scratch;
    const segments segs = split_view(path, scratch);
    // %-编码时路径解码后的存储（视图指向它；仅本次 dispatch 有效）
    std::string decoded_path;
    const std::string_view req_path = scratch.empty() ? path : std::string_view{};
    if (!scratch.empty()) decoded_path = join_segments(segs);
    std::vector<std::string_view> pp;

    // 先匹配路由，再让 middleware 看到 route_info；404/405 也走全局链。
    const route_item* matched = nullptr;
    if (m) {
        for (const auto& r : routes_) {
            if (r.method != *m) continue;
            if (path_matches(r.segs, segs, pp)) {
                matched = &r;
                break;
            }
        }
    }

    std::string allow;
    if (!matched) {
        for (const auto& r : routes_) {
            std::vector<std::string_view> ignored;
            if (!path_matches(r.segs, segs, ignored)) continue;
            std::string_view name = method_name(r.method);
            if (allow.find(name) != std::string::npos) continue;
            if (!allow.empty()) allow.append(", ");
            allow.append(name);
        }
    }

    Request req{.method = method,
                .path = scratch.empty() ? req_path : std::string_view(decoded_path),
                .query = query,
                .body = body,
                .headers = headers};
    web::request_context ctx(req);
    web::route_info info{};
    if (matched) {
        info = web::route_info{
            matched->method, matched->pattern, matched->bean, matched->handler,
            std::span<const std::string>(matched->middleware)};
        ctx.route = &info;
    }

    auto core = [&]() -> Response {
        if (matched) return matched->h(ctx, pp);

        // 404/405 标记为框架错误；外层 ErrorHandler 可交给应用 mapper。
        web::error_info info;
        info.method = std::string(req.method);
        info.path = std::string(req.path);
        if (!allow.empty()) {
            info.status = 405;
            info.code = "method_not_allowed";
            info.message = "method not allowed";
            info.headers.emplace_back("Allow", std::move(allow));
        } else {
            info.status = 404;
            info.code = "not_found";
            info.message = "not found";
        }
        Response response = web::default_error_response(info);
        ctx.put(std::move(info));
        return response;
    };

    try {
        return run_chain(0, ctx, core);
    } catch (const web::error& e) {
        auto info = e.info();
        if (info.method.empty()) info.method = std::string(req.method);
        if (info.path.empty()) info.path = std::string(req.path);
        if (info.route_pattern.empty() && ctx.route)
            info.route_pattern = std::string(ctx.route->pattern);
        if (!info.cause) info.cause = std::current_exception();
        Response response = web::default_error_response(info);
        ctx.put(std::move(info));
        return response;
    } catch (const validation::invalid& e) {
        web::error_info info;
        info.status = 400;
        info.code = "validation_failed";
        info.message = "validation failed";
        info.method = std::string(req.method);
        info.path = std::string(req.path);
        if (ctx.route) info.route_pattern = std::string(ctx.route->pattern);
        for (const auto& field : e.errors())
            info.fields.push_back({field.field, field.message});
        info.cause = std::current_exception();
        Response response = web::default_error_response(info);
        ctx.put(std::move(info));
        return response;
    } catch (const std::exception& e) {
        web::error_info info;
        info.status = 500;
        info.code = "internal";
        info.message = e.what();
        info.method = std::string(req.method);
        info.path = std::string(req.path);
        if (ctx.route) info.route_pattern = std::string(ctx.route->pattern);
        info.cause = std::current_exception();
        Response response = web::default_error_response(info);
        ctx.put(std::move(info));
        return response;
    } catch (...) {
        web::error_info info;
        info.status = 500;
        info.code = "internal";
        info.message = "internal error";
        info.method = std::string(req.method);
        info.path = std::string(req.path);
        if (ctx.route) info.route_pattern = std::string(ctx.route->pattern);
        info.cause = std::current_exception();
        Response response = web::default_error_response(info);
        ctx.put(std::move(info));
        return response;
    }
}
