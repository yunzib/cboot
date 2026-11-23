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

namespace orm {

namespace {

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
    pg_statement(PGconn* conn, std::string sql)
        : conn_(conn), sql_(std::move(sql)) {}

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

        result_ = PQexecParams(conn_, sql_.c_str(),
                               int(params_.size()), nullptr,
                               values.empty() ? nullptr : values.data(),
                               nullptr, nullptr, 0);
        check_result(conn_, result_);
        row_ = -1;
    }

    void ensure_row() const {
        if (!result_ || row_ < 0 || row_ >= PQntuples(result_))
            throw error("cboot.orm: no current row");
    }

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
    explicit pg_connection(PGconn* conn) : conn_(conn) {}
    ~pg_connection() override {
        if (conn_) PQfinish(conn_);
    }

    std::unique_ptr<statement> prepare(std::string_view sql) override {
        return std::make_unique<pg_statement>(conn_, std::string(sql));
    }

    void execute(std::string_view sql) override {
        PGresult* r = PQexec(conn_, std::string(sql).c_str());
        check_result(conn_, r);
        PQclear(r);
    }

    /** @brief PostgreSQL 走 RETURNING，EntityManager 不会调用这个入口 */
    std::int64_t last_insert_id() override { return 0; }

private:
    PGconn* conn_;
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
        return std::make_unique<pg_connection>(conn);
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
        } catch (...) {
            std::lock_guard guard(mu);
            idle.clear();
            total = 0;
            throw;
        }
    }

    /** @brief 关闭池；忙连接在归还时销毁 */
    void close() {
        std::lock_guard guard(mu);
        closed = true;
        idle.clear();
        total = 0;
        cv.notify_all();
    }

    /** @brief 连接归还；closed 时直接销毁 */
    void release(pg_connection* c) {
        std::unique_ptr<pg_connection> p(c);
        std::lock_guard guard(mu);
        if (closed) return;
        idle.push_back(std::move(p));
        cv.notify_one();
    }

    /** @brief 租借连接；池满时等待 acquire_timeout_ms */
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
                lock.unlock();
                auto* raw = c.release();
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
