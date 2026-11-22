// [[=Named{}]] 限定符冒烟测试
//
// 覆盖：
//   - 同一基类的多个 bean（[[=Bean{"name"}]]）
//   - 构造器参数 [[=Named{"..."}]] 按名字注入
//   - 未限定参数注入唯一默认实例（[[=Bean{}]] 无名）
//   - get<T>(name) / get<T>() / get_at<I>()
import cboot.core.annotations;
import cboot.core.context;
import std;

struct DataSource {
    virtual ~DataSource() = default;
    virtual int id() const = 0;
};

struct [[=Bean{"primary"}]] PrimarySource : DataSource {
    int id() const override { return 1; }
};

struct [[=Bean{"replica"}]] ReplicaSource : DataSource {
    int id() const override { return 2; }
};

struct [[=Bean{}]] DefaultSource : DataSource {
    int id() const override { return 0; }
};

struct [[=Bean{}]] NamedConsumer {
    NamedConsumer([[=Named{"primary"}]] DataSource& p,
                  [[=Named{"replica"}]] DataSource& r)
        : primary(p), replica(r) {}
    int sum() const { return primary.id() + replica.id(); }
    DataSource& primary;
    DataSource& replica;
};

struct [[=Bean{}]] DefaultConsumer {
    explicit DefaultConsumer(DataSource& ds) : ds_(ds) {}
    int id() const { return ds_.id(); }
    DataSource& ds_;
};

int main() {
    context<PrimarySource, ReplicaSource, DefaultSource,
            NamedConsumer, DefaultConsumer> ctx;

    if (ctx.get<NamedConsumer>().sum() != 3) return 1;
    if (ctx.get<DefaultConsumer>().id() != 0) return 2;

    if (ctx.get<DataSource>("primary").id() != 1) return 3;
    if (ctx.get<DataSource>("replica").id() != 2) return 4;
    if (ctx.get<DataSource>().id() != 0) return 5;

    if (ctx.get_at<0>().id() != 1) return 6;
    if (ctx.get_at<1>().id() != 2) return 7;

    std::println("NAMED SMOKE: PASS");
    return 0;
}
