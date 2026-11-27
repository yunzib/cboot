/**
 * @file ratelimit.cppm
 * @brief 请求限流 — 固定窗口限速中间件（路由级 / 客户端级）
 *
 * 生产单体与微服务网关前的第一道流量保护：
 *   - 维度：route（路由模板）/ client_ip / route_ip（默认）/ global
 *   - 算法：固定窗口计数（简单、够用；不做令牌桶/漏桶，避免额外线程）
 *   - 超限：429 + Retry-After + JSON body，并计入
 *     `web_rate_limited_total{limiter,route}` 指标
 *   - 客户端地址：`web::remote_addr` 属性优先（server 层未来注入），
 *     否则 `X-Forwarded-For` 首跳 / `X-Real-IP`；都没有记 "unknown"
 *     ——直连部署时 "unknown" 会成为共享桶，属已知边界
 *
 * 用法一：全局中间件（所有路由按客户端 IP 限速）
 *   struct [[=Bean{}]] [[=web::Middleware{.order = -800}]] GlobalRateLimit {
 *       web::RateLimiter limiter{
 *           web::rate_limit_options{.limit = 1000, .window_ms = 1000,
 *                                   .key = web::rate_limit_key::client_ip}};
 *       Response handle(web::request_context& rc, const web::next_fn& next) {
 *           return limiter.handle(rc, next);
 *       }
 *   };
 *
 * 用法二：路由级（只有带 [[=UseMiddleware{"api"}]] 的路由才走）
 *   把上面的 name 改成 "api"，并在 handler 上加该标签。
 *
 * 测试性：构造时可注入时钟（now_ms 回调），窗口推进无需真实等待。
 */
export module cboot.web.ratelimit;

import std;
import cboot.core.metrics;       // web_rate_limited_total
import cboot.web.http;           // Response / Request
import cboot.web.middleware;     // request_context / next_fn / route_info

export namespace web {

namespace detail {

/**
 * @brief 限流超限计数器缓存：key = limiter 名 + '\x1f' + 路由模板
 *
 * 系列对象终身有效（注册表不删系列），缓存避免每次超限都做字符串
 * 拼接 + 注册表查表；超限本身已属低频路径，这里只做最省事的复用。
 */
inline metrics::counter& limited_counter(const std::string& limiter,
                                         const std::string& route) {
    struct cache {
        std::mutex mu;
        std::unordered_map<std::string, metrics::counter*> counters;
    };
    static cache c;
    std::string key = limiter;
    key += '\x1f';
    key += route;
    {
        std::scoped_lock lock(c.mu);
        if (auto it = c.counters.find(key); it != c.counters.end())
            return *it->second;
    }
    auto& counter = metrics::global().get_counter(
        "web_rate_limited_total",
        {metrics::label("limiter", limiter), metrics::label("route", route)});
    std::scoped_lock lock(c.mu);
    c.counters.emplace(std::move(key), &counter);
    return counter;
}

} // namespace detail

/** @brief 限流维度 */
enum class rate_limit_key {
    route,       // 每条路由共享一个桶（保护下游）
    client_ip,   // 每个客户端 IP 一个桶
    route_ip,    // 每客户端 IP × 路由一个桶（默认；防单点刷单接口）
    global,      // 进程一个桶（总闸）
};

/** @brief 固定窗口限流配置 */
struct rate_limit_options {
    int limit{0};              // 每窗口允许请求数；<= 0 关闭限流
    int window_ms{1000};       // 窗口长度（毫秒；<= 0 视为 1000）
    rate_limit_key key{rate_limit_key::route_ip};
    std::string name{"default"};  // 指标标签，多限流器时区分
};

/**
 * @brief 客户端地址属性（框架保留）
 *
 * server 层尚未注入；中间件/测试可提前 ctx.put<web::remote_addr>(...)
 * 覆盖 header 推断。值形如 "203.0.113.9"（不含端口）。
 */
struct remote_addr {
    std::string value;
};

/**
 * @brief 客户端 IP 推断：属性 → X-Forwarded-For 首跳 → X-Real-IP → unknown
 *
 * 只取 X-Forwarded-For 的第一个地址（最左为原始客户端，由前置代理追加）。
 * 直连部署没有这些头时返回 "unknown"（共享桶；见文件头边界）。
 */
inline std::string client_ip(const web::request_context& ctx) {
    if (const auto* a = ctx.find<web::remote_addr>()) {
        if (!a->value.empty()) return a->value;
    }
    if (auto xff = ctx.request.header("X-Forwarded-For")) {
        std::string_view v = *xff;
        if (const std::size_t comma = v.find(','); comma != std::string_view::npos)
            v = v.substr(0, comma);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
            v.remove_suffix(1);
        if (!v.empty()) return std::string(v);
    }
    if (auto real = ctx.request.header("X-Real-IP")) {
        std::string s(*real);
        if (!s.empty()) return s;
    }
    return "unknown";
}

/**
 * @brief 固定窗口限流器（可拷贝，计数状态共享）
 *
 * 计数表：`unordered_map<key, window{start_ms, count}>` + mutex。
 * 过期窗口惰性清理（表超过 1024 项时顺带全量清扫），避免内存无界增长。
 * 线程安全：handle 可并发调用。
 */
class RateLimiter {
public:
    /** @brief 默认时钟（steady_clock 毫秒） */
    static std::int64_t default_now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /** @brief 构造；now_ms 仅测试注入，生产用默认 steady_clock */
    explicit RateLimiter(rate_limit_options opt,
                         std::function<std::int64_t()> now_ms = default_now_ms)
        : opt_(std::move(opt)),
          state_(std::make_shared<state>()),
          now_ms_(std::move(now_ms)) {
        if (opt_.window_ms <= 0) opt_.window_ms = 1000;
    }

    /**
     * @brief 限流检查 → 放行或 429
     *
     * 超限时抛 web::error 还是直接返回？这里直接返回 429 响应：
     * 限流是预期流量控制而非异常，不触发认证/错误事件日志噪声。
     */
    Response handle(web::request_context& ctx, const web::next_fn& next) {
        if (opt_.limit <= 0) return next();

        const std::string key = make_key(ctx);
        const std::int64_t now = now_ms_();
        const std::int64_t window = opt_.window_ms;

        bool limited = false;
        std::int64_t retry_s = 0;
        {
            std::scoped_lock lock(state_->mu);
            auto& w = state_->windows[key];   // 首次访问创建（start 初始 0）
            if (w.start_ms == 0 || now - w.start_ms >= window) {
                w.start_ms = now;
                w.count = 0;
            }
            if (w.count >= opt_.limit) {
                limited = true;
                const std::int64_t elapsed = now - w.start_ms;
                const std::int64_t remain = window - elapsed;
                retry_s = remain > 0 ? (remain + 999) / 1000 : 1;
            } else {
                ++w.count;
            }
            if (state_->windows.size() > 1024) prune(now, window);
        }

        if (!limited) return next();

        // 超限指标：limiter 名称 + 路由模板（无路由记 "-"）
        detail::limited_counter(opt_.name, route_label(ctx)).inc();

        std::string body;
        body += "{\"error\":\"rate_limited\",\"retry_after_s\":";
        body += std::to_string(retry_s);
        body += "}";
        Response r;
        r.status = 429;
        r.body = std::move(body);
        r.header("Retry-After", std::to_string(retry_s));
        return r;
    }

private:
    struct window {
        std::int64_t start_ms = 0;
        int count = 0;
    };

    struct state {
        std::mutex mu;
        std::unordered_map<std::string, window> windows;
    };

    static std::string route_label(const web::request_context& ctx) {
        return ctx.route ? std::string(ctx.route->pattern) : std::string("-");
    }

    std::string make_key(const web::request_context& ctx) const {
        const std::string route = route_label(ctx);
        switch (opt_.key) {
        case rate_limit_key::global:
            return "*";
        case rate_limit_key::route:
            return "route:" + route;
        case rate_limit_key::client_ip:
            return "ip:" + client_ip(ctx);
        case rate_limit_key::route_ip:
            return "route:" + route + "|ip:" + client_ip(ctx);
        }
        return "*";
    }

    /** @brief 过期窗口清扫（调用方持锁） */
    void prune(std::int64_t now, std::int64_t window) {
        for (auto it = state_->windows.begin(); it != state_->windows.end();) {
            if (now - it->second.start_ms >= window)
                it = state_->windows.erase(it);
            else
                ++it;
        }
    }

    rate_limit_options opt_;
    std::shared_ptr<state> state_;   // 拷贝共享计数：同一限流器的多个实例同桶
    std::function<std::int64_t()> now_ms_;
};

} // namespace web
