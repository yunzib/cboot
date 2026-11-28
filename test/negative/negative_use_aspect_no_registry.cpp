// 负面用例（预期编译失败）：用了 [[=web::UseAspect]] 却用无注册表的 mount
// 诊断：cboot.web: [[=web::UseAspect]] needs the registry mount
import cboot.app;
import cboot.web.http;

struct [[=Bean{}]] Api {
    [[=Get{"/x"}]]
    [[=web::UseAspect{"timing"}]]
    Response x() { return Response::text("x"); }
};

int main() {
    Api api;
    router r;
    r.mount(api);        // ← 应编译失败：UseAspect 需要注册表版 mount
    return r.dispatch("GET", "/x", "").status == 200 ? 0 : 1;
}
