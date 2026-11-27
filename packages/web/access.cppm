/**
 * @file access.cppm
 * @brief 请求关联层 — request_id 解析/生成 + 访问日志（access log）中间件
 *
 * 微服务/k8s 前置能力的公共基座：
 *   - 每个请求分配 request_id（优先复用客户端 X-Request-Id），
 *     写入 request_context 属性与日志 MDC，下游所有日志自动带上
 *     request_id（json_layout 会输出为独立字段）；
 *   - 响应回写 X-Request-Id 头，客户端/网关可带回上游做全链路关联
 *     （微服务化后升级为 trace_id 透传的接缝）；
 *   - 请求结束时输出一行 JSON 访问日志（method/path/status/duration_ms/
 *     request_id/route），对标 nginx access log / Spring Boot access log。
 *
 * 顺序：access_middleware_order(-950) 位于 CORS(-1000) 内层、
 * ErrorHandler(-900) 外层——错误转换后的最终状态码与总耗时都能被记录；
 * CORS 预检 204 在外层短路，不产生访问日志（噪声）。
 *
 * 开关：不新增配置键。用 log_component 的 logger_levels 前缀覆盖：
 *   { "log": { "logger_levels": { "cboot.web.access": "off" } } }
 * 即可关闭访问日志（级别判定在消息格式化之前）。
 *
 * 指标：同一中间件顺带记录 Prometheus 系列到 metrics::global()：
 *   http_requests_total{method,route,status}（status 为 2xx..5xx 大类）
 *   http_request_duration_ms{method,route}（固定桶直方图）
 * app 自动挂载 GET /metrics 渲染注册表（见 cboot.app）。
 */
export module cboot.web.access;

import std;
import cboot.log;                // logger / MDC（logging::context::scope）
import cboot.json;               // write_escaped（JSON 转义）
import cboot.core.metrics;       // metrics::global()（请求指标）
import cboot.web.http;           // Request / Response
import cboot.web.middleware;     // request_context / next_fn

export namespace web {

/** @brief 访问日志中间件的推荐顺序：CORS 内层、ErrorHandler 外层 */
inline constexpr int access_middleware_order = -950;

/** @brief request_id 的类型化属性（request_context::get/find 用） */
struct request_id {
    std::string value;
};

/**
 * @brief 访问日志中间件 — request_id 关联 + 单行 JSON access log
 *
 * 无状态、无锁：可直接按值构造（app 启动时自动安装，见 cboot.app）。
 * 手工挂载（测试/独立 server）：
 *   web::AccessLog access;
 *   s.use("", web::access_middleware_order,
 *         [&access](web::request_context& rc, const web::next_fn& next) {
 *             return access.handle(rc, next);
 *         });
 */
class AccessLog {
public:
    /**
     * @brief 处理一次请求：解析/生成 request_id → 放行 → 记录访问日志
     *
     * MDC 作用域仅覆盖本请求（logging::context::scope 析构自动恢复），
     * 因请求处理在同一线程内同步完成，线程本地 MDC 不会串请求。
     * 异常一律由内层 ErrorHandler 兜底，这里不捕获——保证耗时口径
     * 覆盖错误转换路径。
     * 同时记录请求指标（http_requests_total / http_request_duration_ms，
     * 系列 key 带路由模板标签，404 用 "-"，避免任意路径的标签爆炸）。
     */
    Response handle(web::request_context& ctx, const web::next_fn& next) const {
        const auto start = std::chrono::steady_clock::now();

        // 1) request_id：优先复用客户端提供值（须可打印、≤128 字符，
        //    防日志注入）；否则本地生成 128 位随机十六进制。
        std::string id = client_request_id(ctx.request);
        if (id.empty()) id = generate_request_id();

        // 2) 属性 + MDC：下游 middleware / handler / 错误日志全部携带
        ctx.put(web::request_id{id});
        logging::context::scope mdc("request_id", id);

        // 3) 放行（内层含 ErrorHandler，异常不会逃逸到这里）
        Response response = next();

        // 4) 回写响应头：网关/客户端拿到同一 id 参与全链路关联
        response.header("X-Request-Id", id);

        // 5) 访问日志（info 级；可按 logger 前缀覆盖/关闭）
        const auto end = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start);
        auto access_log = logging::get("cboot.web.access");
        if (access_log.enabled(logging::level::info)) {
            access_log.log(logging::level::info,
                           format_entry(ctx, response, id, ms.count()));
        }

        // 6) 请求指标：系列指针按 "方法 路由模板" 缓存，热路径无查表/无字符串
        //    拼接（未命中才进注册表建系列）。状态标签用大类（2xx..5xx）
        //    控制基数；路由未命中记 "-"（任意路径不进标签）。
        record_metrics(ctx, response, ms);
        return response;
    }

private:
    /** @brief 每个方法+路由的系列缓存（5 个状态大类计数器 + 1 个直方图） */
    struct route_series {
        std::string method;
        std::string route;
        std::array<metrics::counter*, 5> by_class{};   // 1xx..5xx → 下标 0..4
        metrics::histogram* duration = nullptr;
    };

    /**
     * @brief 记录请求指标（计数 + 耗时直方图）
     *
     * 缓存命中路径：一次哈希查找 + 短临界区 + 两次原子加——足够便宜，
     * 可默认开启。标签顺序固定 method/route/status（渲染稳定可 diff）。
     */
    void record_metrics(const web::request_context& ctx,
                        const Response& response,
                        std::chrono::milliseconds ms) const {
        // 404/405 无路由模板：用 "-" 占位（路径本身是任意输入，不能当标签）
        const std::string_view route =
            ctx.route ? ctx.route->pattern : std::string_view{"-"};
        std::string key;
        key.reserve(ctx.request.method.size() + route.size() + 2);
        key.append(ctx.request.method);
        key += ' ';
        key.append(route);

        const route_series* series = nullptr;
        {
            std::scoped_lock lock(cache_mu_);
            auto& entry = cache_[std::move(key)];
            if (!entry.duration) {
                // 首次见到该方法+路由：在全局注册表建系列
                entry.method = std::string(ctx.request.method);
                entry.route = std::string(route);
                const std::string method_label =
                    metrics::label("method", ctx.request.method);
                const std::string route_label = metrics::label("route", route);
                for (int cls = 0; cls < 5; ++cls) {
                    const std::string status_label = metrics::label(
                        "status", std::to_string(cls + 1) + "xx");
                    entry.by_class[cls] = &metrics::global().get_counter(
                        "http_requests_total",
                        {method_label, route_label, status_label});
                }
                entry.duration = &metrics::global().get_histogram(
                    "http_request_duration_ms", {method_label, route_label});
            }
            series = &entry;
        }

        const int cls = std::clamp(response.status / 100, 1, 5) - 1;
        series->by_class[static_cast<std::size_t>(cls)]->inc();
        series->duration->observe(static_cast<double>(ms.count()));
    }

    /** @brief 读取并校验 X-Request-Id；非法/缺失返回空串 */
    static std::string client_request_id(const Request& req) {
        if (auto v = req.header("X-Request-Id")) {
            if (v->empty() || v->size() > 128) return {};
            for (char c : *v)
                if (c < 0x21 || c > 0x7e) return {};   // 可打印 ASCII，无空白
            return std::string(*v);
        }
        return {};
    }

    /** @brief 生成 128 位随机 id（32 个十六进制字符） */
    static std::string generate_request_id() {
        // thread_local：热路径无锁；random_device 提供进程外熵源
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::string out;
        out.reserve(32);
        for (int i = 0; i < 2; ++i)
            out += std::format("{:016x}", rng());
        return out;
    }

    /** @brief 组装单行 JSON 访问日志（字段定序，便于采集端解析） */
    static std::string format_entry(const web::request_context& ctx,
                                    const Response& response,
                                    const std::string& id, long long ms) {
        std::string out;
        out.reserve(96 + ctx.request.path.size());
        out += "{\"method\":";
        json::write_escaped(out, ctx.request.method);
        out += ",\"path\":";
        json::write_escaped(out, ctx.request.path);
        out += ",\"status\":";
        out += std::to_string(response.status);
        out += ",\"duration_ms\":";
        out += std::to_string(ms);
        out += ",\"request_id\":";
        json::write_escaped(out, id);
        if (ctx.route) {                     // 命中路由时记录模板（含参数段名）
            out += ",\"route\":";
            json::write_escaped(out, ctx.route->pattern);
        }
        out += "}";
        return out;
    }

    // 系列缓存：key = "METHOD 路由模板"；const handle 下的可变态
    mutable std::mutex cache_mu_;
    mutable std::unordered_map<std::string, route_series> cache_;
};

} // namespace web
