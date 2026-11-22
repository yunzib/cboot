// cboot.aop 冒烟测试
// 验证：洋葱顺序（enter→handler→leave）/ 短路（Auth 拒绝）/ 异常捕获 /
//       call_info 方法名反射 / 非 web 场景直用 aspect_chain / 概念约束
import cboot.aop;
import cboot.json;
import cboot.web.http;
import cboot.web.routing;
import std;

using Log = std::vector<std::string>;

// ── 切面定义（结构类型，无继承；可有状态）──

struct Trace {                          // 记录进出（最外层）
    std::shared_ptr<Log> log;
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        log->push_back("enter:" + std::string(ci.bean) + "." + std::string(ci.method));
        Response r = next();
        log->push_back("leave:" + std::string(ci.method) + ":"
                       + std::to_string(r.status));
        return r;
    }
};

struct Auth {                           // 鉴权（可短路）
    bool allow;
    std::shared_ptr<Log> log;
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        if (!allow) {
            log->push_back("deny:" + std::string(ci.method));
            Response r = Response::text("403 forbidden");
            r.status = 403;
            return r;
        }
        return next();
    }
};

struct CatchAll {                       // 异常 → 500（吃掉异常）
    std::shared_ptr<Log> log;
    template <class Next>
    Response around(const call_info& ci, Next&& next) {
        try {
            return next();
        } catch (const std::exception& e) {
            log->push_back(std::string("caught:") + e.what());
            Response r = Response::text("500 handled by aspect");
            r.status = 500;
            return r;
        }
    }
};

struct TraceAny {                       // 泛型切面：返回类型随 next（非 web 场景）
    std::shared_ptr<Log> log;
    template <class Next>
    auto around(const call_info& ci, Next&& next) {
        log->push_back("enter:" + std::string(ci.bean) + "." + std::string(ci.method));
        auto r = next();
        log->push_back("leave:" + std::string(ci.method));
        return r;
    }
};

// ── 被切的目标 ──

struct Api {
    [[=Get{"/hello/{name}"}]]
    Response hello(std::string_view name) {
        return json_response(json::value("hi " + std::string(name)));
    }

    [[=Get{"/boom"}]]
    Response boom() { throw std::runtime_error("kaboom"); }
};

int main() {
    auto log = std::make_shared<Log>();
    Api api;

    // ── 1. 正常路径：外层先入后出 ──
    router r1;
    r1.mount(api, Trace{log}, Auth{true, log});
    auto resp = r1.dispatch("GET", "/hello/bob", "");
    if (resp.status != 200) return 1;
    if (json::parse(resp.body).as_string() != "hi bob") return 2;
    const Log expected_ok = {"enter:Api.hello", "leave:hello:200"};
    if (*log != expected_ok) {
        for (auto& e : *log) std::println("  got: {}", e);
        return 3;
    }

    // ── 2. 短路：Auth 拒绝，Trace 仍观察到进出 ──
    router r2;
    log->clear();
    r2.mount(api, Trace{log}, Auth{false, log});
    resp = r2.dispatch("GET", "/hello/bob", "");
    if (resp.status != 403) return 4;
    const Log expected_deny = {"enter:Api.hello", "deny:hello", "leave:hello:403"};
    if (*log != expected_deny) return 5;

    // ── 3. 异常：CatchAll 捕获 handler 抛出的异常 ──
    router r3;
    log->clear();
    r3.mount(api, CatchAll{log});
    resp = r3.dispatch("GET", "/boom", "");
    if (resp.status != 500) return 6;
    if (log->size() != 1 || log->at(0) != "caught:kaboom") return 7;

    // ── 4. 非 web 直用：aspect_chain 包裹任意可调用（泛型切面）──
    log->clear();
    aspect_chain<TraceAny> chain{TraceAny{log}};
    int v = chain.run(call_info{.bean = "Calc", .method = "add"},
                      []() -> int { return 41 + 1; });
    if (v != 42) return 8;
    if (log->size() != 2 || log->at(0) != "enter:Calc.add"
        || log->at(1) != "leave:add") return 9;

    // ── 5. 概念约束（编译期校验切面契约）──
    static_assert(AspectFor<Trace, Response, std::function<Response()>>);
    static_assert(AspectFor<Auth, Response, std::function<Response()>>);
    static_assert(AspectFor<TraceAny, int, std::function<int()>>);

    std::println("AOP SMOKE: PASS");
    return 0;
}
