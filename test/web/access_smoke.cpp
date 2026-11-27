/**
 * @file access_smoke.cpp
 * @brief web::AccessLog 冒烟测试 — request_id 关联 + JSON 访问日志
 *
 * 覆盖点：
 *   1. 客户端 X-Request-Id（合法值）→ 复用并回写响应头
 *   2. 无头/非法头（控制字符）→ 本地生成 32 位十六进制 id
 *   3. request_id 进入 request_context 属性与日志 MDC（下游可见）
 *   4. 访问日志单行 JSON：method/path/status/duration_ms/request_id/route
 *   5. 404 也产生访问日志（最终状态码口径）
 *   6. 顺序：AccessLog(-950) 在 ErrorHandler(-900) 外层（此处用探针验证包裹方向）
 */
import std;
import cboot.json;
import cboot.log;
import cboot.web.http;
import cboot.web.middleware;   // request_context / next_fn
import cboot.web.access;       // AccessLog / request_id
import cboot.web.routing;      // router

namespace {

// 捕获 sink：把格式化后的日志行收进 vector 供断言
class capture_sink final : public logging::sink {
public:
    void write(logging::level, std::string_view line) override {
        std::scoped_lock lock(m_);
        lines_.emplace_back(line);
    }
    std::vector<std::string> lines() const {
        std::scoped_lock lock(m_);
        return lines_;
    }

private:
    mutable std::mutex m_;
    std::vector<std::string> lines_;
};

struct UserApi {
    [[=Get{"/users/{id}"}]]
    Response get(int id) {
        Response r;
        r.body = "{\"id\":" + std::to_string(id) + "}";
        return r;
    }
};

// 探针中间件（order 0，最内层）：验证属性 + MDC 已就位
struct probe {
    std::string ctx_id;
    std::string mdc_id;

    Response handle(web::request_context& rc, const web::next_fn& next) {
        ctx_id = rc.get<web::request_id>().value;
        if (auto mdc = logging::context::get("request_id"))
            mdc_id = std::string(*mdc);
        return next();
    }
};

bool contains(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int main() {
    // 捕获日志：替换默认 console sink，避免噪声且可断言
    auto sink = std::make_shared<capture_sink>();
    logging::clear_sinks();
    logging::add_sink(sink);
    logging::set_level(logging::level::info);

    router r;
    UserApi api;
    r.mount(api);
    probe p;
    r.use("", 0, [&p](web::request_context& rc, const web::next_fn& next) {
        return p.handle(rc, next);
    });
    web::AccessLog access;
    r.use("", web::access_middleware_order,
          [&access](web::request_context& rc, const web::next_fn& next) {
              return access.handle(rc, next);
          });

    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::println("FAIL: {}", what);
            ++failures;
        }
    };

    // ── 1/3：合法客户端 id → 复用；属性与 MDC 下游可见 ──
    {
        Response resp = r.dispatch("GET", "/users/42", "",
                                   "Host: x\r\nX-Request-Id: abc-123\r\n\r\n");
        check(resp.status == 200, "client id: status 200");
        bool echoed = false;
        for (const auto& [name, value] : resp.headers)
            if (name == "X-Request-Id" && value == "abc-123") echoed = true;
        check(echoed, "client id echoed in X-Request-Id header");
        check(p.ctx_id == "abc-123", "request_context property carries id");
        check(p.mdc_id == "abc-123", "MDC carries request_id");
    }

    // ── 2：无头 → 生成 32 位十六进制 ──
    {
        Response resp = r.dispatch("GET", "/users/7", "", "Host: x\r\n\r\n");
        std::string gen;
        for (const auto& [name, value] : resp.headers)
            if (name == "X-Request-Id") gen = value;
        check(gen.size() == 32, "generated id is 32 hex chars");
        check(std::ranges::all_of(gen, [](char c) {
                  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
              }),
              "generated id is hex");
        check(gen != "abc-123", "generated id differs per request");
    }

    // ── 2b：非法头（控制字符）→ 不信任客户端，改用生成值 ──
    {
        Response resp = r.dispatch("GET", "/users/8", "",
                                   "Host: x\r\nX-Request-Id: bad\x01id\r\n\r\n");
        std::string got;
        for (const auto& [name, value] : resp.headers)
            if (name == "X-Request-Id") got = value;
        check(got != "bad\x01id" && got.size() == 32,
              "invalid client id replaced by generated");
    }

    // ── 4：访问日志单行 JSON 字段齐全（含 route 模板）──
    {
        Response resp = r.dispatch("GET", "/users/9", "", "Host: x\r\n\r\n");
        std::string rid;
        for (const auto& [name, value] : resp.headers)
            if (name == "X-Request-Id") rid = value;
        const auto lines = sink->lines();
        check(!lines.empty(), "access log line emitted");
        if (!lines.empty()) {
            const std::string& entry = lines.back();
            check(contains(entry, R"("method":"GET")"), "log has method");
            check(contains(entry, R"("path":"/users/9")"), "log has path");
            check(contains(entry, R"("status":200)"), "log has status");
            check(contains(entry, R"("duration_ms":)"), "log has duration_ms");
            check(contains(entry, "\"request_id\":\"" + rid + "\""),
                  "log has matching request_id");
            check(contains(entry, R"("route":"/users/{id}")"),
                  "log has route pattern");
            check(contains(entry, "\n") == false || entry.back() == '\n',
                  "entry is a single line");
        }
    }

    // ── 5：404 也产生访问日志（最终状态码口径）──
    {
        const std::size_t before = sink->lines().size();
        Response resp = r.dispatch("GET", "/nope", "", "Host: x\r\n\r\n");
        check(resp.status == 404, "unknown path returns 404");
        const auto lines = sink->lines();
        check(lines.size() == before + 1, "404 also logged");
        if (lines.size() == before + 1)
            check(contains(lines.back(), R"("status":404)"),
                  "404 logged with final status");
    }

    // ── 6：包裹方向 —— AccessLog 在内层中间件外（探针 id 先于日志写入）──
    {
        const auto lines = sink->lines();
        bool order_ok = false;
        for (const auto& entry : lines)
            if (contains(entry, "\"request_id\":\"" + p.ctx_id + "\""))
                order_ok = true;
        check(order_ok, "innermost probe id visible in access log");
    }

    // 恢复默认 console（不影响后续测试进程行为）
    logging::clear_sinks();
    if (failures == 0) std::println("access_smoke: all checks passed");
    return failures == 0 ? 0 : 1;
}
