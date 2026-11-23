// cboot.config 冒烟测试
// 验证：JSON 展平 / 前缀绑定 / 嵌套聚合 / optional 缺键 / vector 两种形态
//       ${ENV:def} 占位符 / overlay 覆盖 / 非法值 fail-loud
#include <cstdlib>                       // _putenv_s（非模块包含，测试 TU 合法）
import cboot.config;
import cboot.core.annotations;
import cboot.core.context;
import std;

enum class Verb { Quiet, Normal, Verbose };

struct Pool {
    int max{};
    std::optional<int> min;
    bool operator==(const Pool&) const = default;
};
// 通用配置 bean：任意结构体 + 前缀（Spring @ConfigurationProperties 同形）
struct [[=Config{"db"}]] BindDb {
    std::string host;
    int port{};
    std::vector<std::string> replicas;
};

struct BindUser {                       // 直接注入配置 bean
    BindUser(BindDb& c) : db(c) {}
    BindDb& db;
};

struct CfgUser {                        // 依赖注入：配置为内建依赖
    CfgUser(const config::properties& cfg)
        : pool(cfg.get_expanded("db.pool.size").value_or("0")) {}
    std::string pool;
};

struct Db {
    std::string host;
    int port{};
    Pool pool;
    Verb verb{};
    std::vector<std::string> replicas;
    std::vector<int> timeouts;
    bool operator==(const Db&) const = default;
};

int main() {
    // ── 1. JSON 展平 + 前缀绑定 ──
    auto props = config::properties::from_json(R"({
        "db": {
            "host": "${CBT_HOST:localhost}",
            "port": 5432,
            "pool": {"max": 16, "min": 4},
            "verb": "Normal",
            "replicas": ["r1.example.com", "r2.example.com"],
            "timeouts": "100,200,300"
        }
    })");

    Db db = config::bind<Db>(props, "db");
    if (db.host != "localhost" || db.port != 5432 || db.pool.max != 16
        || db.pool.min != 4 || db.verb != Verb::Normal) return 1;

    // vector：索引键（replicas.0/1）与逗号形式（timeouts）
    if (db.replicas.size() != 2 || db.replicas[1] != "r2.example.com") return 2;
    if (db.timeouts.size() != 3 || db.timeouts[2] != 300) return 3;

    // ── 2. 环境变量占位符（覆盖默认值）──
    _putenv_s("CBT_HOST", "db.prod.internal");
    Db db2 = config::bind<Db>(props, "db");
    if (db2.host != "db.prod.internal") return 4;

    // ── 3. overlay（高优先级源覆盖）+ 缺键语义 ──
    config::properties override_;
    override_.set("db.port", "6543");
    props.overlay(override_);
    Db db3 = config::bind<Db>(props, "db");
    if (db3.port != 6543 || db3.pool.max != 16) return 5;

    config::properties empty;
    Db db4 = config::bind<Db>(empty, "db");        // 全缺 → 默认值
    if (!db4.host.empty() || db4.pool.min.has_value() || !db4.replicas.empty()) return 6;

    // ── 4. 非法值 fail-loud ──
    bool threw = false;
    try {
        config::properties bad;
        bad.set("db.port", "not_a_number");
        (void)config::bind<Db>(bad, "db");
    } catch (const std::runtime_error&) { threw = true; }
    if (!threw) return 7;

    // ── 4.1 多键错误聚合 ──
    try {
        config::properties bad;
        bad.set("db.port", "bad");
        bad.set("db.pool.max", "also_bad");
        (void)config::bind<Db>(bad, "db");
        return 14;
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        if (msg.find("db.port") == std::string::npos
            || msg.find("db.pool.max") == std::string::npos)
            return 15;
    }

    // ── 4.2 CLI 解析 ──
    char a0[] = "app";
    char a1[] = "--config=prod.json";
    char a2[] = "--server.port=9000";
    char a3[] = "--db.host=db.prod";
    char* av[] = {a0, a1, a2, a3};
    auto cli = config::parse_command_line(4, av);
    if (cli.config_path != "prod.json") return 16;
    if (cli.values.get_raw("server.port").value_or("") != "9000") return 17;
    if (cli.values.get_raw("db.host").value_or("") != "db.prod") return 18;

    // ── 4.3 CB_ 环境变量映射 ──
#if defined(_WIN32)
    _putenv_s("CB_SERVER_PORT", "9010");
#else
    setenv("CB_SERVER_PORT", "9010", 1);
#endif
    auto env = config::environment("CB_");
    if (env.get_raw("server.port").value_or("") != "9010") return 19;

    // ── 5. 容器注入：bean 构造器取 const config::properties& ──
    config::properties inj;
    inj.set("db.pool.size", "32");
    context<CfgUser> ctx(&inj);
    if (ctx.get<CfgUser>().pool != "32") return 8;

    // 未提供配置 → 运行时明确报错
    bool cfg_threw = false;
    try { context<CfgUser> bad(nullptr); }
    catch (const std::runtime_error&) { cfg_threw = true; }
    if (!cfg_threw) return 9;

    // ── 6. 通用配置 bean（[[=Config{"prefix"}]]）──
    config::properties bound;
    bound.set("db.host", "h1");
    bound.set("db.port", "7777");
    bound.set("db.replicas.0", "r1");
    context<BindDb, BindUser> bc(&bound);
    if (bc.get<BindDb>().host != "h1" || bc.get<BindDb>().port != 7777) return 10;
    if (bc.get<BindDb>().replicas.size() != 1) return 11;
    if (&bc.get<BindUser>().db != &bc.get<BindDb>()) return 12;   // 同一实例

    // 配置 bean 缺配置 → 运行时明确报错
    bool bind_threw = false;
    try { context<BindDb> nb(nullptr); }
    catch (const std::runtime_error&) { bind_threw = true; }
    if (!bind_threw) return 13;

    std::println("CONFIG SMOKE: PASS");
    return 0;
}
