// 定时任务冒烟测试
//
// 覆盖：固定延迟调度、异常不杀死调度线程、析构停止调度、Profile 跳过。
import cboot.core.annotations;   // Bean / Profile
import cboot.core.context;        // context / hook_scope
import cboot.schedule;            // Scheduled / schedule_hook
import cboot.config;
import std;

class [[=Bean{}]] Tick {
public:
    [[=Scheduled{20}]] void tick() { ++count; }
    static inline std::atomic<int> count{0};
};

class [[=Bean{}]] Boom {
public:
    [[=Scheduled{.fixed_delay_ms = 20}]] void boom() {
        if (!threw.exchange(true)) throw std::runtime_error("boom");
    }
    static inline std::atomic<bool> threw{false};
};

class [[=Profile{"dev"}]] DevTick {
public:
    [[=Scheduled{10}]] void tick() { ++count; }
    static inline std::atomic<int> count{0};
};

int main() {
    // 1. 定时执行 + 异常隔离：Boom 第一次抛异常，Tick 仍持续执行
    {
        Tick::count = 0;
        Boom::threw = false;
        context<Boom, Tick> ctx;
        // 调度器是组件 hook：必须在 context 构造完成后安装
        hook_scope<decltype(ctx), schedule_hook> hooks(ctx);
        for (int i = 0; i < 200 && Tick::count < 2; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (Tick::count < 2) return 1;
    }

    // 2. 容器析构后调度停止
    {
        const int before = Tick::count;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (Tick::count != before) return 2;
    }

    // 3. Profile 未激活：定时任务不注册
    {
        DevTick::count = 0;
        config::properties prod;
        prod.set("cboot.profiles.active", "prod");
        context<DevTick> ctx(&prod);
        hook_scope<decltype(ctx), schedule_hook> hooks(ctx);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (DevTick::count != 0) return 3;
    }

    std::println("SCHEDULED SMOKE: PASS");
    return 0;
}
