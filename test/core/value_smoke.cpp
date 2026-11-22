// [[=Value{}]] 标量配置注入冒烟测试
//
// 覆盖：string / int / double / bool / enum / const& 参数；
//       缺配置、缺键、非法值、非法枚举名的运行时诊断。
import cboot.core.annotations;
import cboot.core.context;
import cboot.config;
import std;

enum class Mode { Fast, Slow };

struct [[=Bean{}]] AppCfg {
    AppCfg([[=Value{"app.name"}]] std::string name,
           [[=Value{"app.port"}]] int port,
           [[=Value{"app.ratio"}]] double ratio,
           [[=Value{"app.debug"}]] bool debug,
           [[=Value{"app.mode"}]] Mode mode,
           [[=Value{"app.alias"}]] const std::string& alias)
        : name_(std::move(name)), port_(port), ratio_(ratio),
          debug_(debug), mode_(mode), alias_(alias) {}

    std::string name_;
    int port_;
    double ratio_;
    bool debug_;
    Mode mode_;
    std::string alias_;
};

static config::properties valid_props() {
    config::properties p;
    p.set("app.name", "demo");
    p.set("app.port", "8081");
    p.set("app.ratio", "0.75");
    p.set("app.debug", "true");
    p.set("app.mode", "Slow");
    p.set("app.alias", "alias-value");
    return p;
}

int main() {
    auto props = valid_props();
    context<AppCfg> ctx(&props);
    const AppCfg& c = ctx.get<AppCfg>();
    if (c.name_ != "demo") return 1;
    if (c.port_ != 8081) return 2;
    if (c.ratio_ != 0.75) return 3;
    if (!c.debug_) return 4;
    if (c.mode_ != Mode::Slow) return 5;
    if (c.alias_ != "alias-value") return 6;

    // 缺配置
    bool threw = false;
    try { context<AppCfg> none(nullptr); }
    catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 7;

    // 缺键
    config::properties only;
    only.set("app.name", "demo");
    threw = false;
    try { context<AppCfg> bad(&only); }
    catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 8;

    // 数值非法
    auto bad_num = valid_props();
    bad_num.set("app.port", "not-a-number");
    threw = false;
    try { context<AppCfg> bad(&bad_num); }
    catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 9;

    // 枚举名非法
    auto bad_enum = valid_props();
    bad_enum.set("app.mode", "Turbo");
    threw = false;
    try { context<AppCfg> bad(&bad_enum); }
    catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 10;

    std::println("VALUE SMOKE: PASS");
    return 0;
}
