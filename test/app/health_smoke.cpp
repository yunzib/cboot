// 健康检查 / 运维端点冒烟测试
//
// 覆盖：/health 200/503、JSON 字段、/beans 仅 dev、bean 清单与依赖；
//      k8s 三探针 /livez（依赖 DOWN 仍 200）、/readyz（DOWN→503）、
//      /startupz（200）。
import cboot.app;
import std;

class [[=Bean{}]] Downable {
public:
    explicit Downable(health_registry& h) : h_(h) {}
    void fail() { h_.set_unhealthy("database down"); }
    void recover() { h_.set_healthy(); }
private:
    health_registry& h_;
};

class [[=Bean{}]] Ok {
public:
    int value() const { return 42; }
};

int main() {
    // 1. 默认健康：/health 200 + JSON
    context<Downable, Ok> ctx;
    health_endpoint ep(ctx);
    router r;
    r.mount(ep);

    auto h = r.dispatch("GET", "/health", "");
    if (h.status != 200) return 1;
    auto v = json::parse(h.body);
    if (v.find("status")->as_string() != "UP") return 2;
    if (v.find("beans")->as_int() != 2) return 3;
    if (v.find("startup_ms")->as_int() < 0) return 4;

    // 2. 非 dev：/beans 404
    if (r.dispatch("GET", "/beans", "").status != 404) return 5;

    // 3. 标记 DOWN：/health 503 + reason
    ctx.get<Downable>().fail();
    auto down = r.dispatch("GET", "/health", "");
    if (down.status != 503) return 6;
    auto dv = json::parse(down.body);
    if (dv.find("status")->as_string() != "DOWN") return 7;
    if (dv.find("reason")->as_string() != "database down") return 8;

    // 4. 恢复
    ctx.get<Downable>().recover();
    if (r.dispatch("GET", "/health", "").status != 200) return 9;

    // 4b. k8s 三探针：正常态全 200
    if (r.dispatch("GET", "/livez", "").status != 200) return 15;
    if (r.dispatch("GET", "/startupz", "").status != 200) return 16;
    {
        auto ready = r.dispatch("GET", "/readyz", "");
        if (ready.status != 200) return 17;
        if (json::parse(ready.body).find("status")->as_string() != "READY")
            return 18;
    }

    // 4c. 依赖 DOWN：liveness 仍 200（不误重启），readiness 503 + reason
    ctx.get<Downable>().fail();
    if (r.dispatch("GET", "/livez", "").status != 200) return 19;
    {
        auto ready = r.dispatch("GET", "/readyz", "");
        if (ready.status != 503) return 20;
        auto rv = json::parse(ready.body);
        if (rv.find("status")->as_string() != "NOT_READY") return 21;
        if (rv.find("reason")->as_string() != "database down") return 22;
    }
    ctx.get<Downable>().recover();

    // 5. dev profile：/beans 返回 bean 清单
    config::properties dev;
    dev.set("cboot.profiles.active", "dev");
    context<Ok, Downable> ctx2(&dev);
    health_endpoint ep2(ctx2);
    router r2;
    r2.mount(ep2);

    auto beans = r2.dispatch("GET", "/beans", "");
    if (beans.status != 200) return 10;
    auto arr = json::parse(beans.body);
    if (!arr.is_array() || arr.as_array().size() != 2) return 11;

    bool found = false;
    for (const auto& e : arr.as_array()) {
        if (e.find("type")->as_string() == "Downable") {
            found = true;
            if (!e.find("active")->as_bool()) return 12;
            if (!e.find("deps")->is_array()) return 13;
        }
    }
    if (!found) return 14;

    std::println("HEALTH SMOKE: PASS");
    return 0;
}
