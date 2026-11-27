/**
 * @file metrics_smoke.cpp
 * @brief metrics 注册表 + 请求指标集成冒烟测试
 *
 * 覆盖点：
 *   1. registry：counter/histogram 系列（同 key 幂等）、累加、固定桶分桶
 *   2. render_prometheus：TYPE 行、counter 值、histogram bucket/sum/count、
 *      标签渲染（含 le 追加与 +Inf 桶）
 *   3. AccessLog 集成：http_requests_total{method,route,status 大类}、
 *      http_request_duration_ms{method,route}；404 记 route="-"
 *   4. 指标与访问日志开关解耦（日志静音时指标仍记录）
 */
import std;
import cboot.core.metrics;
import cboot.log;
import cboot.web.http;
import cboot.web.middleware;
import cboot.web.access;
import cboot.web.routing;

namespace {

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t count(const std::string& hay, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

struct UserApi {
    [[=Get{"/users/{id}"}]]
    Response get(int id) {
        Response r;
        r.body = "{\"id\":" + std::to_string(id) + "}";
        return r;
    }
};

} // namespace

int main() {
    // 日志静音（覆盖点 4：日志关闭不影响指标）
    logging::clear_sinks();
    logging::set_level(logging::level::off);

    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::println("FAIL: {}", what);
            ++failures;
        }
    };

    // ── 1/2：注册表语义与 Prometheus 渲染 ──
    metrics::global().clear();
    {
        auto& c = metrics::global().get_counter(
            "demo_total", {metrics::label("a", "1")});
        c.inc();
        c.inc(2);
        // 同 key 幂等：拿到的是同一系列
        auto& again = metrics::global().get_counter(
            "demo_total", {metrics::label("a", "1")});
        again.inc();
        check(c.value() == 4, "counter accumulates and is idempotent per key");

        auto& h = metrics::global().get_histogram(
            "latency_ms", {metrics::label("route", "/x")});
        h.observe(3);        // 落 le=5 桶
        h.observe(60);       // 落 le=100 桶
        h.observe(20000);    // 超全部有限上界 → +Inf 桶
        check(h.count() == 3, "histogram count");

        auto& g = metrics::global().get_gauge(
            "pool_conns", {metrics::label("state", "idle")});
        g.set(3);
        // gauge 同 key 幂等
        metrics::global().get_gauge(
            "pool_conns", {metrics::label("state", "idle")}).set(4);
        check(g.value() == 4, "gauge set and is idempotent per key");
    }

    // ── 3：AccessLog 请求指标集成 ──
    {
        router r;
        UserApi api;
        r.mount(api);
        web::AccessLog access;
        r.use("", web::access_middleware_order,
              [&access](web::request_context& rc, const web::next_fn& next) {
                  return access.handle(rc, next);
              });

        (void)r.dispatch("GET", "/users/1", "", "Host: x\r\n\r\n");
        (void)r.dispatch("GET", "/users/2", "", "Host: x\r\n\r\n");
        (void)r.dispatch("GET", "/nope", "", "Host: x\r\n\r\n");   // 404

        auto& after = metrics::global().get_counter(
            "http_requests_total",
            {metrics::label("method", "GET"),
             metrics::label("route", "/users/{id}"),
             metrics::label("status", "2xx")});
        check(after.value() == 2, "http_requests_total 2xx counted per route");

        auto& miss = metrics::global().get_counter(
            "http_requests_total",
            {metrics::label("method", "GET"), metrics::label("route", "-"),
             metrics::label("status", "4xx")});
        check(miss.value() == 1, "404 counted with route=\"-\"");

        auto& dur = metrics::global().get_histogram(
            "http_request_duration_ms",
            {metrics::label("method", "GET"),
             metrics::label("route", "/users/{id}")});
        check(dur.count() == 2, "duration histogram observed per request");
    }

    // ── 2：Prometheus 文本渲染 ──
    {
        const std::string text = metrics::global().render_prometheus();
        check(contains(text, "# TYPE demo_total counter\n"), "TYPE counter line");
        check(contains(text, "demo_total{a=\"1\"} 4\n"), "counter sample line");
        check(contains(text, "# TYPE latency_ms histogram\n"), "TYPE histogram");
        check(contains(text, "latency_ms_bucket{route=\"/x\",le=\"5\"} 1\n"),
              "bucket le=5");
        check(contains(text, "latency_ms_bucket{route=\"/x\",le=\"100\"} 2\n"),
              "cumulative buckets");
        check(contains(text, "latency_ms_bucket{route=\"/x\",le=\"+Inf\"} 3\n"),
              "+Inf bucket");
        check(contains(text, "latency_ms_count{route=\"/x\"} 3\n"),
              "histogram count line");
        check(contains(text, "latency_ms_sum{route=\"/x\"} 20063\n"),
              "histogram sum line");
        check(contains(text, "# TYPE pool_conns gauge\n"),
              "TYPE gauge line");
        check(contains(text, "pool_conns{state=\"idle\"} 4\n"),
              "gauge sample line");
        check(contains(
                  text,
                  "http_requests_total{method=\"GET\",route=\"-\",status=\"4xx\"} 1\n"),
              "request counter rendered with fixed label order");
        // TYPE 行按指标名只输出一次（同名的多个系列共享）
        check(count(text, "# TYPE http_requests_total counter\n") == 1,
              "TYPE line emitted once per metric name");
    }

    metrics::global().clear();
    if (failures == 0) std::println("metrics_smoke: all checks passed");
    return failures == 0 ? 0 : 1;
}
