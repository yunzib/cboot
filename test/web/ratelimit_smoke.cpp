/**
 * @file ratelimit_smoke.cpp
 * @brief web::RateLimiter 冒烟测试（纯逻辑，无 socket）
 *
 * 覆盖点：
 *   1. 固定窗口：limit 内放行、超限 429 + Retry-After + JSON body
 *   2. 窗口推进（注入时钟）后恢复
 *   3. route_ip 维度：同 IP 不同路由各自计数
 *   4. client_ip 维度：不同 IP 独立桶（X-Forwarded-For）
 *   5. global 维度：跨 IP 共享一个桶
 *   6. remote_addr 属性优先于 header
 *   7. limit<=0 关闭限流
 *   8. 超限指标 web_rate_limited_total 递增
 */
import std;
import cboot.core.metrics;
import cboot.json;
import cboot.web.http;
import cboot.web.middleware;
import cboot.web.ratelimit;
import cboot.web.routing;

namespace {

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

struct Api {
    [[=Get{"/a"}]]
    Response a() {
        Response r;
        r.body = "ok";
        return r;
    }

    [[=Get{"/b"}]]
    Response b() {
        Response r;
        r.body = "ok";
        return r;
    }
};

/** @brief 可注入时钟：测试直接推进时间，不 sleep */
struct fake_clock {
    std::int64_t now = 1'700'000'000'000;
    std::int64_t operator()() const { return now; }
};

std::string xff(std::string_view ip) {
    std::string headers = "Host: x\r\nX-Forwarded-For: ";
    headers += ip;
    headers += "\r\n\r\n";
    return headers;
}

} // namespace

int main() {
    metrics::global().clear();

    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::println("FAIL: {}", what);
            ++failures;
        }
    };

    Api api;
    router r;
    r.mount(api);

    fake_clock clock;
    // 全局安装：模拟中间件链中的限流层（order 0，最内层）
    web::RateLimiter limiter(
        web::rate_limit_options{
            .limit = 2,
            .window_ms = 1000,
            .key = web::rate_limit_key::route_ip,
            .name = "test",
        },
        [&clock] { return clock.now; });
    r.use("", 0, [&limiter](web::request_context& rc, const web::next_fn& next) {
        return limiter.handle(rc, next);
    });

    // ── 1：limit=2/窗口 → 前两个 200，第三个 429 ──
    check(r.dispatch("GET", "/a", "", xff("1.1.1.1")).status == 200, "req 1 pass");
    check(r.dispatch("GET", "/a", "", xff("1.1.1.1")).status == 200, "req 2 pass");
    {
        Response limited = r.dispatch("GET", "/a", "", xff("1.1.1.1"));
        check(limited.status == 429, "req 3 blocked");
        bool retry = false;
        for (const auto& [name, value] : limited.headers)
            if (name == "Retry-After") retry = true;
        check(retry, "Retry-After header present");
        auto v = json::parse(limited.body);
        check(v.find("error")->as_string() == "rate_limited", "429 body error");
        check(v.find("retry_after_s")->as_int() >= 1, "429 retry_after_s");
    }

    // ── 2：窗口推进后恢复 ──
    clock.now += 1000;
    check(r.dispatch("GET", "/a", "", xff("1.1.1.1")).status == 200,
          "window elapsed resets");

    // ── 3：同 IP 不同路由各自计数 ──
    clock.now += 1000;
    check(r.dispatch("GET", "/a", "", xff("2.2.2.2")).status == 200, "route a 1");
    check(r.dispatch("GET", "/b", "", xff("2.2.2.2")).status == 200,
          "route b independent");

    // ── 4：不同 IP 独立桶 ──
    clock.now += 1000;
    check(r.dispatch("GET", "/a", "", xff("3.3.3.3")).status == 200, "ip a 1");
    check(r.dispatch("GET", "/a", "", xff("4.4.4.4")).status == 200, "ip b 1");

    // ── 5/7：global 维度与关闭开关 ──
    {
        web::RateLimiter gl(web::rate_limit_options{
            .limit = 1,
            .window_ms = 1000,
            .key = web::rate_limit_key::global,
            .name = "global",
        }, [&clock] { return clock.now; });
        router r2;
        r2.mount(api);
        r2.use("", 0, [&gl](web::request_context& rc, const web::next_fn& next) {
            return gl.handle(rc, next);
        });
        check(r2.dispatch("GET", "/a", "", xff("9.9.9.9")).status == 200,
              "global first");
        check(r2.dispatch("GET", "/b", "", xff("8.8.8.8")).status == 429,
              "global shares bucket across ips");
    }
    {
        web::RateLimiter off(web::rate_limit_options{
            .limit = 0, .window_ms = 1000,
            .key = web::rate_limit_key::client_ip, .name = "off",
        }, [&clock] { return clock.now; });
        router r3;
        r3.mount(api);
        r3.use("", 0, [&off](web::request_context& rc, const web::next_fn& next) {
            return off.handle(rc, next);
        });
        for (int i = 0; i < 5; ++i)
            check(r3.dispatch("GET", "/a", "", xff("5.5.5.5")).status == 200,
                  "limit<=0 disables");
    }

    // ── 6：remote_addr 属性优先 ──
    {
        static const Request req{.method = "GET", .path = "/a",
                                 .headers = "X-Forwarded-For: 7.7.7.7\r\n"};
        static web::request_context ctx(req);
        ctx.put(web::remote_addr{"203.0.113.9"});
        check(web::client_ip(ctx) == "203.0.113.9",
              "remote_addr attribute wins over header");
    }
    {
        static const Request req{.method = "GET", .path = "/a",
                                 .headers = "X-Forwarded-For: 7.7.7.7, 10.0.0.1\r\n"};
        static web::request_context ctx(req);
        check(web::client_ip(ctx) == "7.7.7.7", "first XFF hop used");
    }

    // ── 8：超限指标 ──
    {
        const std::string text = metrics::global().render_prometheus();
        check(contains(text, "web_rate_limited_total"), "metric series exists");
        check(contains(text, "limiter=\"test\""), "limiter label present");
    }

    metrics::global().clear();
    if (failures == 0) std::println("ratelimit_smoke: all checks passed");
    return failures == 0 ? 0 : 1;
}
