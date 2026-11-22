// 负面用例（预期编译失败）：带路由方法但未标注 [[=Bean{}]]
// 诊断："cboot: 'Api' has route methods but lacks [[=Bean{}]] annotation"
import cboot.app;
import cboot.web.http;

struct Api {
    [[=Get{"/x"}]]
    Response x() { return Response::text("x"); }
};

int main() { return run(19000); }
