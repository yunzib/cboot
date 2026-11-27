/**
 * @file postgres.cpp
 * @brief libpq 驱动实现
 *
 * 只在这里接触 libpq 类型：
 *   - pg_connection / pg_statement
 *   - PgDataSource::impl 连接池
 *
 * 模块接口不 include libpq，消费者不需要 libpq 头文件；
 * 但运行时需要 libpq.dll 在 PATH 或 exe 同目录。
 */

module;

#include <libpq-fe.h>

module cboot.orm.postgres;

import std;
import cboot.orm.driver;        // DataSource / statement / to_bind_text
import cboot.core.context;      // health_registry
import cboot.core.metrics;      // 池/查询指标（Prometheus）
import cboot.log;               // 慢 SQL warn 日志

namespace orm {

namespace {

/** @brief 驱动名标签（指标维度；进程内固定 → 系列只建一次） */
metrics::gauge& pool_gauge(const char* state) {
    // 函数级静态：首次取用建系列，此后零查表（系列终身有效）
    static metrics::gauge& idle = metrics::global().get_gauge(
        "orm_pool_connections",
        {metrics::label("dialect", "postgres"), metrics::label("state", "idle")});
    static metrics::gauge& active = metrics::global().get_gauge(
        "orm_pool_connections",
        {metrics::label("dialect", "postgres"), metrics::label("state", "active")});
    return std::string_view(state) == "idle" ? idle : active;
}

metrics::counter& query_counter() {
    static metrics::counter& c = metrics::global().get_counter(
        "orm_queries_total", {metrics::label("dialect", "postgres")});
    return c;
}

metrics::histogram& query_histogram() {
    static metrics::histogram& h = metrics::global().get_histogram(
        "orm_query_duration_ms", {metrics::label("dialect", "postgres")});
    return h;
}

metrics::counter& reconnect_counter() {
    static metrics::counter& c = metrics::global().get_counter(
        "orm_pool_reconnects_total", {metrics::label("dialect", "postgres")});
    return c;
}

/**
 * @brief 查询结束后的统一上报：耗时直方图 + 计数 + 慢 SQL 日志
 *
 * 慢 SQL 日志截取 SQL 前 256 字符（防御超长语句刷屏）；
 * 阈值 <= 0 表示关闭日志（指标照记）。
 */
void report_query(const PgProps* cfg, std::string_view sql,
                  std::chrono::steady_clock::time_point start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    query_counter().inc();
    query_histogram().observe(static_cast<double>(ms.count()));
    if (cfg && cfg->slow_query_ms > 0
        && ms.count() >= cfg->slow_query_ms) {
        auto log = logging::get("cboot.orm.pg");
        log.warn("slow query {} ms: {:.256}", ms.count(), sql);
    }
}

/**
 * @brief 参数值 → libpq 文本
 *
 * 第一版用文本格式：PostgreSQL 按目标列类型转换。
 * 后续可切二进制格式减少转换开销。
 */
std::optional<std::string> bind_text(const db_value& v) {
    return to_bind_text(v);
}

/**
 * @brief 执行结果错误检查
 *
 * 错误信息来自 libpq；不会包含密码（conninfo 不进入错误文本）。
 */
void check_result(PGconn* conn, PGresult* r) {
    if (!r) throw error("cboot.orm: " + std::string(PQerrorMessage(conn)));
    const auto st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        std::string msg = PQresultErrorMessage(r);
        PQclear(r);
        throw error("cboot.orm: " + msg);
    }
}

/**
 * @brief PostgreSQL statement
 *
 * 第一次 next() 时执行；文本参数按位置绑定。
 * 结果只在 statement 生命周期内有效。
 */
class pg_statement final : public statement {
public:
    pg_statement(PgProps* cfg, PGconn* conn, std::string sql)
        : cfg_(cfg), conn_(conn), sql_(std::move(sql)) {}

    ~pg_statement() override {
        if (result_) PQclear(result_);
    }

    void bind(std::size_t index, const db_value& value) override {
        if (executed_)
            throw error("cboot.orm: cannot bind after execute");
        if (index >= params_.size()) params_.resize(index + 1);
        params_[index] = bind_text(value);
    }

    bool next() override {
        if (!executed_) execute();
        if (!result_) return false;
        if (PQresultStatus(result_) != PGRES_TUPLES_OK) return false;
        if (row_ + 1 >= PQntuples(result_)) return false;
        ++row_;
        return true;
    }

    bool is_null(std::size_t column) override {
        ensure_row();
        return PQgetisnull(result_, row_, int(column)) != 0;
    }

    std::int64_t get_int(std::size_t column) override {
        auto s = get_text(column);
        std::int64_t v{};
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size())
            throw error("cboot.orm: invalid integer column");
        return v;
    }

    double get_double(std::size_t column) override {
        auto s = get_text(column);
        double v{};
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size())
            throw error("cboot.orm: invalid floating column");
        return v;
    }

    std::string_view get_text(std::size_t column) override {
        ensure_row();
        const int c = int(column);
        const char* p = PQgetvalue(result_, row_, c);
        const int n = PQgetlength(result_, row_, c);
        return {p, std::size_t(n)};
    }

    std::size_t column_count() const override {
        return result_ ? std::size_t(PQnfields(result_)) : 0;
    }

    std::string_view column_name(std::size_t column) const override {
        if (!result_ || column >= std::size_t(PQnfields(result_)))
            return {};
        const char* name = PQfname(result_, int(column));
        return name ? std::string_view(name) : std::string_view{};
    }

    std::size_t affected_rows() const override {
        if (!result_) return 0;
        const char* s = PQcmdTuples(result_);
        if (!s || !*s) return 0;
        std::size_t v{};
        auto [p, ec] = std::from_chars(s, s + std::strlen(s), v);
        return (ec == std::errc{} && p == s + std::strlen(s)) ? v : 0;
    }

private:
    void execute() {
        executed_ = true;
        std::vector<const char*> values;
        values.reserve(params_.size());
        for (const auto& p : params_)
            values.push_back(p ? p->c_str() : nullptr);

        // 计时覆盖"发送 + 服务器执行 + 首块返回"（含绑定渲染，不含行遍历）
        const auto start = std::chrono::steady_clock::now();
        result_ = PQexecParams(conn_, sql_.c_str(),
                               int(params_.size()), nullptr,
                               values.empty() ? nullptr : values.data(),
                               nullptr, nullptr, 0);
        check_result(conn_, result_);
        report_query(cfg_, sql_, start);
        row_ = -1;
    }

    void ensure_row() const {
        if (!result_ || row_ < 0 || row_ >= PQntuples(result_))
            throw error("cboot.orm: no current row");
    }

    PgProps* cfg_ = nullptr;
    PGconn* conn_;
    std::string sql_;
    std::vector<std::optional<std::string>> params_;
    PGresult* result_ = nullptr;
    int row_ = -1;
    bool executed_ = false;
};

/** @brief 单个 libpq 连接；由连接池独占租借 */
class pg_connection final : public connection {
public:
    pg_connection(PgProps* cfg, PGconn* conn) : cfg_(cfg), conn_(conn) {}
    ~pg_connection() override {
        if (conn_) PQfinish(conn_);
    }

    std::unique_ptr<statement> prepare(std::string_view sql) override {
        return std::make_unique<pg_statement>(cfg_, conn_, std::string(sql));
    }

    void execute(std::string_view sql) override {
        const auto start = std::chrono::steady_clock::now();
        PGresult* r = PQexec(conn_, std::string(sql).c_str());
        report_query(cfg_, sql, start);
        check_result(conn_, r);
        PQclear(r);
    }

    /** @brief PostgreSQL 走 RETURNING，EntityManager 不会调用这个入口 */
    std::int64_t last_insert_id() override { return 0; }

    /**
     * @brief 本地状态检查（零开销）：连接是否处于 OK 态
     *
     * 只能发现客户端已知的断连（对端 RST 已到达）；服务器悄悄
     * 回收的连接仍显示 OK —— 由 probe() 覆盖。
     */
    bool alive() const { return PQstatus(conn_) == CONNECTION_OK; }

    /**
     * @brief 真实探测（一次往返）：发送空查询并等待服务器响应
     *
     * 空查询返回 PGRES_EMPTY_QUERY，同样证明链路活着；
     * 失败（异常返回值/错误状态/连接断开）一律视为死连接。
     */
    bool probe() {
        PGresult* r = PQexec(conn_, "");
        if (!r) return false;
        const auto st = PQresultStatus(r);
        PQclear(r);
        return st == PGRES_EMPTY_QUERY || st == PGRES_TUPLES_OK
            || st == PGRES_COMMAND_OK;
    }

    /** @brief 是否需要借出前探测：空闲超过 idle_probe_s 且开关打开 */
    bool probe_due() const {
        if (!cfg_ || cfg_->idle_probe_s <= 0) return false;
        const auto idle = std::chrono::steady_clock::now() - last_used_;
        return idle > std::chrono::seconds(cfg_->idle_probe_s);
    }

    /** @brief 记录最近使用时间（借出/归还时调用） */
    void touch() { last_used_ = std::chrono::steady_clock::now(); }

    PgProps* cfg_ = nullptr;

private:
    PGconn* conn_;
    std::chrono::steady_clock::time_point last_used_{
        std::chrono::steady_clock::now()};
};

} // namespace

// ==================== PgDataSource::impl ====================

struct PgDataSource::impl {
    PgProps* cfg = nullptr;
    health_registry* health = nullptr;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::unique_ptr<pg_connection>> idle;
    std::size_t total = 0;
    bool closed = false;

    /**
     * @brief 建立一条 libpq 连接；失败抛 orm::error
     *
     * 用 PQconninfoParse 统一处理 URI 和 keyword=value 两种写法，
     * 再由 PgProps 的 user/password/sslmode/connect_timeout 覆盖：
     *   - password 为空或未展开的 ${PGPASSWORD}：不写 password，
     *     libpq 自动读取 PGPASSWORD 环境变量；
     *   - sslmode 只在 URL/conninfo 没写时补默认值，避免覆盖显式配置。
     */
    std::unique_ptr<pg_connection> connect_one() {
        const std::string url = cfg->url.empty()
            ? "postgresql://localhost/postgres" : cfg->url;

        char* parse_err = nullptr;
        PQconninfoOption* opts = PQconninfoParse(url.c_str(), &parse_err);
        if (!opts) {
            std::string msg = parse_err ? parse_err : "invalid conninfo";
            if (parse_err) PQfreemem(parse_err);
            if (health) health->set_unhealthy("postgres: " + msg);
            throw error("cboot.orm: invalid connection string: " + msg);
        }

        std::vector<std::string> keys;
        std::vector<std::string> vals;
        for (PQconninfoOption* o = opts; o->keyword; ++o) {
            if (!o->val) continue;
            keys.emplace_back(o->keyword);
            vals.emplace_back(o->val);
        }
        PQconninfoFree(opts);

        auto set_opt = [&](std::string_view key, const std::string& val,
                           bool overwrite = true) {
            if (val.empty()) return;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (keys[i] != key) continue;
                if (overwrite) vals[i] = val;
                return;
            }
            keys.emplace_back(key);
            vals.push_back(val);
        };

        set_opt("user", cfg->user);
        if (!cfg->password.empty() && cfg->password.rfind("${", 0) != 0)
            set_opt("password", cfg->password);
        set_opt("sslmode", cfg->sslmode, false);
        if (cfg->connect_timeout_s > 0)
            set_opt("connect_timeout", std::to_string(cfg->connect_timeout_s));

        std::vector<const char*> keywords;
        std::vector<const char*> values;
        keywords.reserve(keys.size() + 1);
        values.reserve(vals.size() + 1);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keywords.push_back(keys[i].c_str());
            values.push_back(vals[i].c_str());
        }
        keywords.push_back(nullptr);
        values.push_back(nullptr);

        PGconn* conn = PQconnectdbParams(keywords.data(), values.data(), 0);
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            std::string msg = conn ? PQerrorMessage(conn) : "PQconnectdbParams failed";
            if (conn) PQfinish(conn);
            if (health) health->set_unhealthy("postgres: " + msg);
            throw error("cboot.orm: connection failed: " + msg);
        }
        PQsetClientEncoding(conn, "UTF8");
        if (health) health->set_healthy();
        return std::make_unique<pg_connection>(cfg, conn);
    }

    /** @brief 打开连接池；失败时已建立连接全部销毁 */
    void open() {
        std::unique_lock lock(mu);
        if (total != 0) return;
        const int n = cfg->pool_size > 0 ? cfg->pool_size : 4;
        lock.unlock();
        try {
            for (int i = 0; i < n; ++i) {
                auto c = connect_one();
                std::lock_guard guard(mu);
                idle.push_back(std::move(c));
                ++total;
            }
            pool_gauge("idle").set(static_cast<double>(idle.size()));
            pool_gauge("active").set(0);
        } catch (...) {
            std::lock_guard guard(mu);
            idle.clear();
            total = 0;
            pool_gauge("idle").set(0);
            throw;
        }
    }

    /** @brief 关闭池；忙连接在归还时销毁 */
    void close() {
        std::lock_guard guard(mu);
        closed = true;
        idle.clear();
        total = 0;
        pool_gauge("idle").set(0);
        cv.notify_all();
    }

    /** @brief 连接归还；closed 时直接销毁 */
    void release(pg_connection* c) {
        std::unique_ptr<pg_connection> p(c);
        std::lock_guard guard(mu);
        pool_gauge("active").set(active_ > 0 ? static_cast<double>(active_ - 1)
                                             : 0.0);
        --active_;
        if (closed) return;
        p->touch();                        // 空闲计时的锚点 = 归还时刻
        idle.push_back(std::move(p));
        pool_gauge("idle").set(static_cast<double>(idle.size()));
        cv.notify_one();
    }

    /**
     * @brief 借出前校验：本地状态 + （空闲过久时）一次真实探测
     *
     * 校验失败 → 销毁死连接并重建（保持池容量不变，业务无感知）；
     * 重建计入 orm_pool_reconnects_total。重建失败按连接失败处理。
     */
    std::unique_ptr<pg_connection> validate_or_reconnect(
        std::unique_ptr<pg_connection> c) {
        if (c->alive() && (!c->probe_due() || c->probe())) return c;
        // 死连接：销毁重建（total 计数不变，容量语义保持）
        reconnect_counter().inc();
        return connect_one();
    }

    /** @brief 租借连接；池满时等待 acquire_timeout_ms；借出前做活性校验 */
    connection_lease acquire() {
        std::unique_lock lock(mu);
        const auto timeout = std::chrono::milliseconds(
            cfg->acquire_timeout_ms > 0 ? cfg->acquire_timeout_ms : 5000);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (true) {
            if (closed)
                throw error("cboot.orm: data source closed");
            if (!idle.empty()) {
                auto c = std::move(idle.back());
                idle.pop_back();
                pool_gauge("idle").set(static_cast<double>(idle.size()));
                lock.unlock();
                try {
                    c = validate_or_reconnect(std::move(c));
                } catch (...) {
                    // 校验后重建失败：归还容量计数，异常上抛
                    std::lock_guard guard(mu);
                    --total;
                    pool_gauge("active").set(static_cast<double>(active_));
                    throw;
                }
                c->touch();
                auto* raw = c.release();
                {
                    std::lock_guard guard(mu);
                    ++active_;
                    pool_gauge("active").set(static_cast<double>(active_));
                }
                return connection_lease(raw, [this](connection* p) {
                    release(static_cast<pg_connection*>(p));
                });
            }
            const int max = cfg->pool_size > 0 ? cfg->pool_size : 4;
            if (int(total) < max) {
                ++total;
                lock.unlock();
                try {
                    auto c = connect_one();
                    auto* raw = c.release();
                    {
                        std::lock_guard guard(mu);
                        ++active_;
                        pool_gauge("active").set(static_cast<double>(active_));
                    }
                    return connection_lease(raw, [this](connection* p) {
                        release(static_cast<pg_connection*>(p));
                    });
                } catch (...) {
                    std::lock_guard guard(mu);
                    --total;
                    throw;
                }
            }
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
                throw error("cboot.orm: acquire connection timeout");
        }
    }

    std::size_t active_ = 0;    // 已借出未归还的连接数（gauge 用）
};

// ==================== PgDataSource ====================

PgDataSource::PgDataSource(PgProps& cfg, health_registry& health)
    : impl_(std::make_unique<impl>()) {
    impl_->cfg = &cfg;
    impl_->health = &health;
}

PgDataSource::~PgDataSource() {
    close();
}

void PgDataSource::open() {
    impl_->open();
}

void PgDataSource::close() {
    impl_->close();
}

connection_lease PgDataSource::acquire() {
    return impl_->acquire();
}

} // namespace orm
