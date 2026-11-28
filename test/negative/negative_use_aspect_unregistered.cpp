// 负面用例（预期编译失败）：[[=web::UseAspect]] 指向未注册的切面名
// 诊断：cboot: [[=web::UseAspect{"timing"}]] matches no bean
//       with [[=web::Aspect{...}]]
import cboot.app;
import cboot.web.http;

struct [[=Bean{}]] Api {
    [[=Get{"/x"}]]
    [[=web::UseAspect{"timing"}]]
    Response x() { return Response::text("x"); }
};

int main() { return run(19002); }
