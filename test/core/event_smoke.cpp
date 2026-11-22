// 事件总线冒烟测试
//
// 覆盖：编译期收集监听者、精确类型匹配、声明顺序、异常隔离、
//       aspect_chain 包裹、Profile 跳过未激活监听者。
import cboot.aop;
import cboot.core.annotations;
import cboot.core.context;
import cboot.config;
import std;

struct OrderPlaced {
    int id{};
};

inline std::vector<std::string> events_log;

class [[=Bean{}]] Audit {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced& e) {
        events_log.push_back("audit:" + std::to_string(e.id));
    }
};

class [[=Bean{}]] Mail {
public:
    [[=EventListener{}]]
    void on(OrderPlaced e) {
        events_log.push_back("mail:" + std::to_string(e.id));
    }
};

class [[=Bean{}]] Other {
public:
    [[=EventListener{}]]
    void on(int v) {
        events_log.push_back("int:" + std::to_string(v));
    }
};

class [[=Bean{}]] Exploder {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced&) {
        throw std::runtime_error("boom");
    }
};

class [[=Bean{}]] OrderService {
public:
    explicit OrderService(event_publisher& bus) : bus_(bus) {}
    event_result place(int id) { return bus_.publish(OrderPlaced{id}); }
private:
    event_publisher& bus_;
};

class [[=Profile{"dev"}]] DevListener {
public:
    [[=EventListener{}]]
    void on(const OrderPlaced& e) {
        events_log.push_back("dev:" + std::to_string(e.id));
    }
};

class EventTrace {
public:
    template <class Next>
    void around(const call_info& ci, Next&& next) {
        events_log.push_back("enter:" + std::string(ci.method));
        next();
        events_log.push_back("leave:" + std::string(ci.method));
    }
};

int main() {
    // 1. 两个监听者，按 bean 清单顺序
    {
        context<Audit, Mail, Other> ctx;
        events_log.clear();
        auto r = ctx.publish(OrderPlaced{7});
        if (r.delivered != 2 || r.failed != 0) return 1;
        if (events_log != std::vector<std::string>{"audit:7", "mail:7"}) return 2;
    }

    // 2. 没有匹配事件类型
    {
        context<Audit, Mail, Other> ctx;
        events_log.clear();
        auto r = ctx.publish(std::string("no listener"));
        if (r.delivered != 0 || r.failed != 0 || !events_log.empty()) return 3;
    }

    // 3. 异常隔离：Exploder 失败后 Audit 仍执行
    {
        context<Exploder, Audit> ctx;
        std::vector<std::string> errors;
        ctx.set_event_error_handler(
            [&](std::string_view bean, std::string_view method,
                std::string_view what) {
                errors.push_back(std::string(bean) + "." + std::string(method)
                                 + ":" + std::string(what));
            });
        events_log.clear();
        auto r = ctx.publish(OrderPlaced{9});
        if (r.delivered != 1 || r.failed != 1) return 4;
        if (events_log != std::vector<std::string>{"audit:9"}) return 5;
        if (errors.size() != 1 || errors[0].find("Exploder.on") == std::string::npos
            || errors[0].find("boom") == std::string::npos) return 6;
    }

    // 4. aspect_chain 包裹每个监听者
    {
        context<Audit, Mail> ctx;
        aspect_chain<EventTrace> chain{EventTrace{}};
        events_log.clear();
        auto r = ctx.publish(OrderPlaced{11},
            [&](std::string_view bean, std::string_view method, auto&& fn) {
                chain.run(call_info{bean, method}, fn);
            });
        if (r.delivered != 2 || r.failed != 0) return 7;
        const std::vector<std::string> expected{
            "enter:on", "audit:11", "leave:on",
            "enter:on", "mail:11", "leave:on"};
        if (events_log != expected) return 8;
    }

    // 5. Profile：未激活 bean 的监听者跳过
    {
        config::properties prod;
        prod.set("cboot.profiles.active", "prod");
        context<DevListener, Audit> ctx(&prod);
        events_log.clear();
        auto r = ctx.publish(OrderPlaced{12});
        if (r.delivered != 1 || r.failed != 0) return 9;
        if (events_log != std::vector<std::string>{"audit:12"}) return 10;
    }

    // 6. 非类事件（int）精确匹配
    {
        context<Audit, Mail, Other> ctx;
        events_log.clear();
        auto r = ctx.publish(42);
        if (r.delivered != 1 || r.failed != 0) return 11;
        if (events_log != std::vector<std::string>{"int:42"}) return 12;
    }

    // 7. 注入式 event_publisher（业务 bean 内部发布）
    {
        context<Audit, Mail, OrderService> ctx;
        events_log.clear();
        auto r = ctx.get<OrderService>().place(5);
        if (r.delivered != 2 || r.failed != 0) return 13;
        if (events_log != std::vector<std::string>{"audit:5", "mail:5"}) return 14;
    }

    std::println("EVENT SMOKE: PASS");
    return 0;
}
