/**
 * @file middleware.cppm
 * @brief 请求级中间件 — request_context / 中间件角色 / 路由标签
 *
 * middleware 是 Bean 的一种角色，不新增组件描述符字段：
 *   - 全局中间件：[[=Middleware{}]]（name 为空）
 *   - 路由中间件：[[=Middleware{"auth"}]] + handler 上的 [[=UseMiddleware{"auth"}]]
 *
 * 中间件在路由匹配后、handler 参数绑定前执行；可读取 headers、
 * 写入 request_context、短路返回 401/403/429，或调用 next() 放行。
 */
export module cboot.web.middleware;

import std;
import cboot.core.annotations;   // bean_name
import cboot.web.http;           // Request / Response / http_method

export namespace web {

/** @brief 中间件角色注解；name 为空表示全局中间件 */
struct Middleware {
    bean_name name{};
    int order{};
};

/** @brief 路由级中间件标签；handler 方法可重复标注 */
struct UseMiddleware {
    bean_name name{};
};

/** @brief 路由元数据；middleware_tags 指向本次路由匹配的存储 */
struct route_info {
    http_method method{};
    std::string_view pattern;
    std::string_view bean;
    std::string_view handler;
    std::span<const std::string> middleware_tags;
};

/**
 * @brief 请求上下文
 *
 * 生命周期仅限一次 dispatch。middleware 可写入类型化属性（如 principal），
 * handler 通过 `const request_context&` 参数读取。
 */
class request_context {
public:
    explicit request_context(const Request& req) : request(req) {}

    const Request& request;
    const route_info* route = nullptr;

    /** @brief 查找类型 T 的属性；不存在返回 nullptr */
    template <class T>
    T* find() {
        for (auto& a : attrs_)
            if (auto* p = std::any_cast<T>(&a)) return p;
        return nullptr;
    }

    template <class T>
    const T* find() const {
        for (const auto& a : attrs_)
            if (const auto* p = std::any_cast<T>(&a)) return p;
        return nullptr;
    }

    /**
     * @brief 取类型 T 的属性；不存在抛 std::logic_error
     *
     * 用于“上游 middleware 保证已写入”的约定（如 AuthGuard → auth::session）。
     * 属性缺失属于框架/配置错误，由 ErrorHandler 统一转 500，避免空指针。
     */
    template <class T>
    const T& get() const {
        if (const auto* p = find<T>()) return *p;
        throw std::logic_error("cboot.web: request_context attribute missing");
    }

    /** @brief 写入/覆盖类型 T 的属性 */
    template <class T>
    void put(T value) {
        for (auto& a : attrs_) {
            if (a.type() == typeid(T)) {
                a = std::move(value);
                return;
            }
        }
        attrs_.emplace_back(std::move(value));
    }

private:
    std::vector<std::any> attrs_;
};

/** @brief 继续执行下一环；不调用即短路 */
using next_fn = std::function<Response()>;

/** @brief 中间件处理器类型擦除形态（启动时注册，非每请求分配） */
using middleware_fn = std::function<Response(request_context&, const next_fn&)>;

/** @brief 中间件 Bean 需要实现 handle(request_context&, const next_fn&) */
template <class M>
concept MiddlewareHandler = requires(M& m, request_context& ctx,
                                     const next_fn& next) {
    { m.handle(ctx, next) } -> std::same_as<Response>;
};

} // namespace web
