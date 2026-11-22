// Profile 条件装配冒烟测试
//
// 覆盖：无 profile（仅默认 bean）、dev/prod 切换、逗号+空格激活列表、
//       未激活 bean 不可 get/get_at、依赖未激活 bean 启动报错。
import cboot.core.annotations;
import cboot.core.context;
import cboot.config;
import std;

class [[=Profile{"dev"}]] DevBean {
public:
    int id() const { return 1; }
};

class [[=Profile{"prod"}]] ProdBean {
public:
    int id() const { return 2; }
};

class [[=Bean{}]] AlwaysBean {
public:
    int id() const { return 3; }
};

class [[=Bean{}]] DevConsumer {
public:
    explicit DevConsumer(DevBean& d) : d_(d) {}
    int id() const { return d_.id(); }
private:
    DevBean& d_;
};

static config::properties with_profile(std::string_view active) {
    config::properties p;
    p.set("cboot.profiles.active", std::string(active));
    return p;
}

int main() {
    // 1. 无激活 profile：只有未标注 Profile 的 bean 生效
    config::properties none;
    context<DevBean, ProdBean, AlwaysBean> c0(&none);
    if (c0.is_active<0>() || c0.is_active<1>() || !c0.is_active<2>()) return 1;
    if (c0.get<AlwaysBean>().id() != 3) return 2;
    bool threw = false;
    try { (void)c0.get<DevBean>(); }
    catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 3;

    // 2. dev：DevBean + AlwaysBean
    auto dev = with_profile("dev");
    context<DevBean, ProdBean, AlwaysBean> c1(&dev);
    if (!c1.is_active<0>() || c1.is_active<1>() || !c1.is_active<2>()) return 4;
    if (c1.get<DevBean>().id() != 1) return 5;
    if (c1.get_at<0>().id() != 1) return 6;

    // 3. prod：ProdBean + AlwaysBean
    auto prod = with_profile("prod");
    context<DevBean, ProdBean, AlwaysBean> c2(&prod);
    if (c2.is_active<0>() || !c2.is_active<1>()) return 7;
    if (c2.get<ProdBean>().id() != 2) return 8;
    threw = false;
    try { (void)c2.get_at<0>(); }
    catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 9;

    // 4. 逗号 + 空格
    auto multi = with_profile("staging, dev");
    context<DevBean, ProdBean, AlwaysBean> c3(&multi);
    if (!c3.is_active<0>() || c3.is_active<1>()) return 10;

    // 5. 依赖未激活 bean：启动期明确报错
    bool dep_threw = false;
    try { context<DevBean, DevConsumer> bad(&prod); }
    catch (const std::runtime_error&) { dep_threw = true; }
    if (!dep_threw) return 11;

    std::println("PROFILE SMOKE: PASS");
    return 0;
}
