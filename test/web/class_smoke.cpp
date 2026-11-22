// class 关键字支持冒烟测试
//
// 验证 C++ 访问控制与反射的边界（GCC16.2 实测）：
//   - class 可直接标注（注解在 class-key 之后）
//   - 私有**数据成员**可反射读写（unchecked）——但 DI 只碰构造器，不碰成员
//   - 构造器 / 生命周期方法 / 路由 handler 必须 public（框架无法越权调用）
import cboot.core.annotations;
import cboot.core.context;
import cboot.json;
import cboot.meta.reflect;   // for_each_member / nth_member（模块导入不传递）
import cboot.web.http;
import cboot.web.routing;
import std;

class [[=Bean{"repo"}]] Repo {
public:
    Repo() = default;                       // public 构造器（注入前提）
    int find(int id) const { return id * 3; }
    [[=PreDestroy{}]] void close() { closed_ = true; }
    static inline bool closed_ = false;

private:
    int hits_ = 0;                          // 私有数据成员：允许（框架不触碰）
};

class [[=Bean{}]] Svc {
public:
    Svc(Repo& repo) : repo_(repo) {}        // 构造器注入（public）
    [[=PostConstruct{}]] void init() { inited_ = true; }
    int calc(int id) { return repo_.find(id); }
    static inline bool inited_ = false;

private:
    Repo& repo_;
};

class Api {
public:
    Api(Svc& svc) : svc_(svc) {}

    [[=Get{"/calc/{id}"}]]                  // class 成员亦可作路由 handler
    Response calc(int id) { return json_response(json::value(svc_.calc(id))); }

private:
    Svc& svc_;
};

// 私有数据成员经 unchecked 反射读写（for_each_member 通用能力）
class Box {
    int a_ = 1;                             // 私有
public:
    int b = 2;                              // 公有
};

int main() {
    // ── 1. class bean：DI + 生命周期 ──
    {
        context<Repo, Svc> ctx;
        if (ctx.get<Svc>().calc(7) != 21) return 1;
        if (!Svc::inited_) return 2;

        // ── 2. class 上的路由 handler ──
        Api api(ctx.get<Svc>());
        router r;
        r.mount(api);
        auto resp = r.dispatch("GET", "/calc/5", "");
        if (resp.status != 200 || json::parse(resp.body).as_int() != 15) return 3;

        // ── 3. 私有成员反射（for_each_member 见全部成员）──
        Box box;
        int sum = 0;
        for_each_member(box, [&](std::string_view, auto& v) { sum += int(v); });
        if (sum != 3) return 4;
        box.[:nth_member<Box, 0>():] = 10;   // 直接 splice 改私有成员
        if (box.[:nth_member<Box, 0>():] != 10) return 5;
    }
    if (!Repo::closed_) return 6;           // PreDestroy（class）

    std::println("CLASS SMOKE: PASS");
    return 0;
}
