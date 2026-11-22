// 定时任务表达式冒烟测试
//
// 覆盖：cron 解析（5/6 段、步长、范围、列表）、每秒调度、hook 析构停止。
import cboot.app;
import cboot.core.annotations;
import cboot.core.context;
import cboot.schedule;
import std;

// 应用选择 schedule 组件后，hooks 清单应包含 schedule_hook
struct ScheduleApp : application<ScheduleApp> {
    using components = type_list<schedule_component>;
};
static_assert(std::is_same_v<
    app_hooks_t<ScheduleApp>, type_list<schedule_hook>>);

// ── 编译期解析 ──────────────────────────────────────────────────────────

static_assert(!parse_cron("* * * * *").has_seconds);
static_assert(parse_cron("* * * * *").minutes == ~std::uint64_t{0} >> (64 - 60));
static_assert(parse_cron("* * * * *").seconds == 1);

static_assert(parse_cron("* * * * * *").has_seconds);
static_assert(parse_cron("*/5 * * * * *").seconds
    == (1ull << 0) | (1ull << 5) | (1ull << 10) | (1ull << 15)
     | (1ull << 20) | (1ull << 25) | (1ull << 30) | (1ull << 35)
     | (1ull << 40) | (1ull << 45) | (1ull << 50) | (1ull << 55));

static_assert(parse_cron("0 0 12 * * 1-5").hours == (1ull << 12));
static_assert(parse_cron("0 0 12 * * 1-5").days_of_week
    == (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 5));

static_assert(parse_cron("0,30 * * * *").minutes
    == (1ull << 0) | (1ull << 30));
static_assert(parse_cron("0 0 1 1 *").months == (1ull << 1));
static_assert(parse_cron("0 0 1 1 7").days_of_week == 1);  // 7 -> 周日

// ── 运行时：每秒 ────────────────────────────────────────────────────────

class [[=Bean{}]] EverySecond {
public:
    [[=Scheduled{.expression = "* * * * * *"}]] void tick() { ++count; }
    static inline std::atomic<int> count{0};
};

int main(int argc, char**) {
    // 编译验证：application::run → boot_runner 携带 schedule_hook
    if (argc > 1000) return ScheduleApp{}.run();

    EverySecond::count = 0;
    context<EverySecond> ctx;
    {
        hook_scope<decltype(ctx), schedule_hook> hooks(ctx);
        for (int i = 0; i < 500 && EverySecond::count < 2; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (EverySecond::count < 2) return 1;
    }

    // hook 析构后调度停止
    const int before = EverySecond::count;
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (EverySecond::count != before) return 2;

    std::println("SCHEDULE EXPR SMOKE: PASS");
    return 0;
}
