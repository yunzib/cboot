/**
 * @file error.cppm
 * @brief 全局错误层 — 统一错误结构、错误处理 middleware 与响应器链
 *
 * 与 Servlet Filter / Spring Security ExceptionTranslationFilter 等价：
 * ErrorHandler 是一个全局 middleware，捕获下游异常并接管 router 标记的
 * 404/405/校验失败，再交给应用注册的 ErrorMapper。
 *
 * 解析阶段的 400/413 发生在进入 middleware 之前（connector 层），
 * 和 Servlet 容器解析错误一样不经过该 middleware。
 */
export module cboot.web.error;

import std;
import cboot.json;
import cboot.validation;      // invalid / field_error
import cboot.web.http;        // Response / json
import cboot.web.middleware;  // request_context / next_fn

export namespace web {

/** @brief 全局错误 middleware 的建议顺序：CORS(-1000) 内层、AuthGuard(100) 外层 */
inline constexpr int error_middleware_order = -900;

/** @brief 字段级错误（校验失败等） */
struct error_field {
    std::string field;
    std::string message;
};

/**
 * @brief 统一错误信息
 *
 * status/code/message 必有；method/path/route_pattern 在能取得请求时填充。
 * headers 用于 Allow / WWW-Authenticate 等协议头。
 */
struct error_info {
    int status{500};
    std::string code{"internal"};
    std::string message;
    std::string method;
    std::string path;
    std::string route_pattern;
    std::string subject;
    std::vector<error_field> fields;
    std::vector<std::pair<std::string, std::string>> headers;
    std::exception_ptr cause;
};

/**
 * @brief 应用可抛出的结构化错误
 *
 * AuthGuard 用它在 middleware 中表达 401/403；ErrorHandler / router
 * 捕获后交给 error_registry。业务 handler 也可以抛它返回任意状态码。
 */
class error : public std::runtime_error {
public:
    explicit error(error_info info)
        : std::runtime_error(info.message.empty() ? info.code : info.message),
          info_(std::move(info)) {}

    const error_info& info() const noexcept { return info_; }

private:
    error_info info_;
};

/** @brief 错误响应器；返回 nullopt 表示继续下一个 mapper */
using error_mapper_fn = std::function<std::optional<Response>(
    const error_info&, const web::request_context&)>;

/** @brief mapper 角色注解；应用 Bean 实现 handle 即可注册 */
struct ErrorMapper {
    int order{};
};

/** @brief ErrorMapper Bean 的 handle 契约 */
template <class M>
concept ErrorMapperHandler = requires(M& m, const error_info& info,
                                      const web::request_context& ctx) {
    { m.handle(info, ctx) } -> std::same_as<std::optional<Response>>;
};

namespace detail {

inline void fill_request(error_info& info, const web::request_context& ctx) {
    if (info.method.empty()) info.method = std::string(ctx.request.method);
    if (info.path.empty()) info.path = std::string(ctx.request.path);
    if (info.route_pattern.empty() && ctx.route)
        info.route_pattern = std::string(ctx.route->pattern);
}

/** @brief 合并协议头（Allow / WWW-Authenticate），不覆盖 mapper 已设置的同名头 */
inline void merge_headers(Response& response, const error_info& info) {
    for (const auto& [name, value] : info.headers) {
        bool exists = false;
        for (const auto& [existing, ignored] : response.headers)
            if (existing == name) {
                exists = true;
                break;
            }
        if (!exists) response.header(name, value);
    }
}

} // namespace detail

/** @brief 默认错误响应（保持既有 400/404/405/413/401/403/500 输出） */
Response default_error_response(const error_info& info) {
    Response r;

    if (info.status == 400 && !info.fields.empty()) {
        json::array items;
        for (const auto& field : info.fields) {
            json::object item;
            item.emplace_back("field", json::value(field.field));
            item.emplace_back("message", json::value(field.message));
            items.emplace_back(json::value(std::move(item)));
        }
        json::object root;
        root.emplace_back("errors", json::value(std::move(items)));
        r = json_response(json::value(std::move(root)));
        r.status = 400;
    } else if (info.status == 401) {
        json::object body;
        body.emplace_back("error", json::value("unauthorized"));
        r = json_response(json::value(std::move(body)));
        r.status = 401;
    } else if (info.status == 403) {
        json::object body;
        body.emplace_back("error", json::value("forbidden"));
        r = json_response(json::value(std::move(body)));
        r.status = 403;
    } else {
        std::string text = std::to_string(info.status);
        if (!info.message.empty()) {
            text += ' ';
            text += info.message;
        }
        r = Response::text(std::move(text));
        r.status = info.status;
    }

    for (const auto& [name, value] : info.headers)
        r.header(name, value);
    return r;
}

/**
 * @brief 错误 mapper 注册表
 *
 * add() 仅启动期调用；respond() 请求期只读，可并发调用。
 * mapper 按 order 升序执行，返回 nullopt 继续；异常被忽略并继续。
 * 协议头统一由 respond 合并，mapper 不必重复设置。
 */
class error_registry {
public:
    void add(int order, error_mapper_fn fn) {
        mappers_.push_back({order, std::move(fn)});
        std::stable_sort(mappers_.begin(), mappers_.end(),
            [](const entry& a, const entry& b) { return a.order < b.order; });
    }

    Response respond(const error_info& info,
                     const web::request_context& ctx) const {
        Response response;
        bool handled = false;
        for (const auto& mapper : mappers_) {
            try {
                if (auto custom = mapper.fn(info, ctx)) {
                    response = std::move(*custom);
                    handled = true;
                    break;
                }
            } catch (...) {
                // 错误响应器自身的异常不能把原始错误变成崩溃/二次错误
            }
        }
        if (!handled) response = default_error_response(info);
        else detail::merge_headers(response, info);

        return response;
    }

    std::size_t size() const { return mappers_.size(); }

private:
    struct entry {
        int order;
        error_mapper_fn fn;
    };

    std::vector<entry> mappers_;
};

/**
 * @brief 全局错误处理 middleware
 *
 * 顺序建议 error_middleware_order（-900）：CORS 内层、AuthGuard 外层。
 * - next() 返回后检查 request_context 中的 error_info 标记（router 404/405 等）
 * - 捕获下游 web::error / validation::invalid / std::exception
 */
class ErrorHandler {
public:
    explicit ErrorHandler(const error_registry& registry) : registry_(registry) {}

    Response handle(web::request_context& ctx, const web::next_fn& next) const {
        try {
            Response response = next();
            if (const auto* marked = ctx.find<error_info>()) {
                auto info = *marked;
                detail::fill_request(info, ctx);
                return registry_.respond(info, ctx);
            }
            return response;
        } catch (const web::error& e) {
            auto info = e.info();
            detail::fill_request(info, ctx);
            if (!info.cause) info.cause = std::current_exception();
            return registry_.respond(info, ctx);
        } catch (const validation::invalid& e) {
            error_info info{
                .status = 400,
                .code = "validation_failed",
                .message = "validation failed",
            };
            for (const auto& field : e.errors())
                info.fields.push_back({field.field, field.message});
            detail::fill_request(info, ctx);
            info.cause = std::current_exception();
            return registry_.respond(info, ctx);
        } catch (const std::exception& e) {
            error_info info{
                .status = 500,
                .code = "internal",
                .message = e.what(),
            };
            detail::fill_request(info, ctx);
            info.cause = std::current_exception();
            return registry_.respond(info, ctx);
        } catch (...) {
            error_info info{
                .status = 500,
                .code = "internal",
                .message = "internal error",
            };
            detail::fill_request(info, ctx);
            info.cause = std::current_exception();
            return registry_.respond(info, ctx);
        }
    }

private:
    const error_registry& registry_;
};

} // namespace web
