/**
 * @file mysql.cpp
 * @brief libmysql 驱动实现
 *
 * 只在这里接触 mysql.h 类型：
 *   - mysql_connection / mysql_statement
 *   - MysqlDataSource::impl 连接池
 *
 * 第一版使用 MySQL 文本协议：statement 绑定参数在 execute 时用
 * mysql_real_escape_string 转义后替换 ?。这样不需要维护 MYSQL_STMT
 * 的结果缓冲，但 SQL 文本中的 ? 全部按参数占位符处理。
 *
 * 模块接口不 include mysql.h，消费者不需要 MySQL 头文件；
 * 运行时需要 libmysql.dll 在 PATH 或 exe 同目录。
 */

module;

// GCC16 模块边界守卫：mysql.h 在 Windows 上会经 windows.h 拉入
// stdlib.h→mm_malloc→xmmintrin，和 import std 的同名声明冲突。
// 与 web/server.cppm 相同，预置 glibc 头/内建函数 guard。
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 1
#define _MM_MALLOC_H_INCLUDED
#define _X86INTRIN_H_INCLUDED
#define _IMMINTRIN_H_INCLUDED
#define _XMMINTRIN_H_INCLUDED
#define _EMMINTRIN_H_INCLUDED
#define _MMINTRIN_H_INCLUDED
#define _PMMINTRIN_H_INCLUDED
#define _CPUID_H_INCLUDED

#define WIN32_LEAN_AND_MEAN
#if defined(_WIN32)
#include <winsock2.h>
#endif
#include <mysql.h>

module cboot.orm.mysql;

import std;
import cboot.orm.driver;        // DataSource / statement / db_value
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
        {metrics::label("dialect", "mysql"), metrics::label("state", "idle")});
    static metrics::gauge& active = metrics::global().get_gauge(
        "orm_pool_connections",
        {metrics::label("dialect", "mysql"), metrics::label("state", "active")});
    return std::string_view(state) == "idle" ? idle : active;
}

metrics::counter& query_counter() {
    static metrics::counter& c = metrics::global().get_counter(
        "orm_queries_total", {metrics::label("dialect", "mysql")});
    return c;
}

metrics::histogram& query_histogram() {
    static metrics::histogram& h = metrics::global().get_histogram(
        "orm_query_duration_ms", {metrics::label("dialect", "mysql")});
    return h;
}

metrics::counter& reconnect_counter() {
    static metrics::counter& c = metrics::global().get_counter(
        "orm_pool_reconnects_total", {metrics::label("dialect", "mysql")});
    return c;
}

/**
 * @brief 查询结束后的统一上报：耗时直方图 + 计数 + 慢 SQL 日志
 *
 * 慢 SQL 日志截取 SQL 前 256 字符（防御超长语句刷屏）；
 * 阈值 <= 0 表示关闭日志（指标照记）。
 */
void report_query(const MysqlProps* cfg, std::string_view sql,
                  std::chrono::steady_clock::time_point start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    query_counter().inc();
    query_histogram().observe(static_cast<double>(ms.count()));
    if (cfg && cfg->slow_query_ms > 0
        && ms.count() >= cfg->slow_query_ms) {
        auto log = logging::get("cboot.orm.mysql");
        log.warn("slow query {} ms: {:.256}", ms.count(), sql);
    }
}

/** @brief 进程级 libmysql 初始化；线程级初始化在 ensure_thread 中完成 */
void ensure_library() {
    static const bool initialized = [] {
        if (mysql_library_init(0, nullptr, nullptr) != 0)
            throw error("cboot.orm: mysql_library_init failed");
        return true;
    }();
    (void)initialized;
}

/** @brief 每个使用 libmysql 的线程调用一次 mysql_thread_init */
struct mysql_thread_guard {
    mysql_thread_guard() {
        if (mysql_thread_init() != 0)
            throw error("cboot.orm: mysql_thread_init failed");
    }
    ~mysql_thread_guard() { mysql_thread_end(); }
    mysql_thread_guard(const mysql_thread_guard&) = delete;
    mysql_thread_guard& operator=(const mysql_thread_guard&) = delete;
};

thread_local mysql_thread_guard tls_thread_guard;

/** @brief 确保当前线程可以使用 libmysql */
void ensure_thread() {
    ensure_library();
    (void)tls_thread_guard;
}

/**
 * @brief 参数值 → MySQL SQL 字面量
 *
 * 字符串用 mysql_real_escape_string 按当前连接字符集转义；
 * 数字/bool 直接生成字面量；nullptr → NULL。
 */
std::string sql_literal(MYSQL* conn, const db_value& v) {
    if (std::holds_alternative<std::nullptr_t>(v)) return "NULL";
    if (auto p = std::get_if<bool>(&v)) return *p ? "1" : "0";
    if (auto p = std::get_if<std::int64_t>(&v)) return std::to_string(*p);
    if (auto p = std::get_if<double>(&v)) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, *p);
        return std::string(buf, r.ptr);
    }
    if (auto p = std::get_if<std::string>(&v)) {
        std::vector<char> escaped(p->size() * 2 + 1);
        const auto n = mysql_real_escape_string(
            conn, escaped.data(), p->data(), static_cast<unsigned long>(p->size()));
        std::string out;
        out.reserve(n + 2);
        out.push_back('\'');
        out.append(escaped.data(), n);
        out.push_back('\'');
        return out;
    }
    return "NULL";
}

/**
 * @brief MySQL statement（文本协议）
 *
 * 第一次 next() 时渲染并执行 SQL；结果集通过 mysql_store_result
 * 全部取回，next() 逐行前进。
 */
class mysql_statement final : public statement {
public:
    mysql_statement(MysqlProps* cfg, MYSQL* conn, std::string sql)
        : cfg_(cfg), conn_(conn), sql_(std::move(sql)) {}

    ~mysql_statement() override {
        if (result_) mysql_free_result(result_);
    }

    void bind(std::size_t index, const db_value& value) override {
        if (executed_)
            throw error("cboot.orm: cannot bind after execute");
        if (index >= params_.size()) params_.resize(index + 1);
        params_[index] = value;
    }

    bool next() override {
        if (!executed_) execute();
        if (!result_) return false;
        row_ = mysql_fetch_row(result_);
        if (!row_) return false;
        lengths_ = mysql_fetch_lengths(result_);
        return true;
    }

    bool is_null(std::size_t column) override {
        ensure_row();
        return row_[column] == nullptr;
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
        const char* p = row_[column];
        if (!p) return {};
        const auto n = lengths_ ? lengths_[column] : std::strlen(p);
        return {p, std::size_t(n)};
    }

    std::size_t column_count() const override {
        return result_ ? std::size_t(mysql_num_fields(result_)) : 0;
    }

    std::string_view column_name(std::size_t column) const override {
        if (!result_ || column >= std::size_t(mysql_num_fields(result_)))
            return {};
        const MYSQL_FIELD* fields = mysql_fetch_fields(result_);
        return fields && fields[column].name
            ? std::string_view(fields[column].name) : std::string_view{};
    }

    std::size_t affected_rows() const override { return affected_rows_; }

private:
    void execute() {
        executed_ = true;
        ensure_thread();

        // 渲染文本 SQL：? 按绑定顺序替换；缺失/多余参数都视为调用错误。
        std::string sql;
        sql.reserve(sql_.size() + params_.size() * 8);
        std::size_t next = 0;
        for (char c : sql_) {
            if (c != '?') {
                sql.push_back(c);
                continue;
            }
            if (next >= params_.size() || !params_[next])
                throw error("cboot.orm: unbound SQL parameter");
            sql += sql_literal(conn_, *params_[next++]);
        }
        if (next != params_.size())
            throw error("cboot.orm: extra SQL parameter");

        // 计时覆盖渲染 + 执行 + 结果集全量取回（store_result）
        const auto start = std::chrono::steady_clock::now();
        if (mysql_real_query(conn_, sql.data(),
                             static_cast<unsigned long>(sql.size())) != 0) {
            report_query(cfg_, sql_, start);
            throw error("cboot.orm: " + std::string(mysql_error(conn_)));
        }

        result_ = mysql_store_result(conn_);
        const auto affected = mysql_affected_rows(conn_);
        affected_rows_ = affected < 0 ? 0 : static_cast<std::size_t>(affected);
        report_query(cfg_, sql_, start);
        if (!result_ && mysql_field_count(conn_) != 0)
            throw error("cboot.orm: " + std::string(mysql_error(conn_)));
    }

    void ensure_row() const {
        if (!result_ || !row_)
            throw error("cboot.orm: no current row");
    }

    MysqlProps* cfg_ = nullptr;
    MYSQL* conn_;
    std::string sql_;
    std::vector<std::optional<db_value>> params_;
    MYSQL_RES* result_ = nullptr;
    MYSQL_ROW row_ = nullptr;
    unsigned long* lengths_ = nullptr;
    std::size_t affected_rows_ = 0;
    bool executed_ = false;
};

/** @brief 单个 libmysql 连接；由连接池独占租借 */
class mysql_connection final : public connection {
public:
    mysql_connection(MysqlProps* cfg, MYSQL* conn) : cfg_(cfg), conn_(conn) {}
    ~mysql_connection() override {
        if (conn_) mysql_close(conn_);
    }

    std::unique_ptr<statement> prepare(std::string_view sql) override {
        ensure_thread();
        return std::make_unique<mysql_statement>(cfg_, conn_, std::string(sql));
    }

    void execute(std::string_view sql) override {
        ensure_thread();
        const auto start = std::chrono::steady_clock::now();
        if (mysql_real_query(conn_, sql.data(),
                             static_cast<unsigned long>(sql.size())) != 0) {
            report_query(cfg_, sql, start);
            throw error("cboot.orm: " + std::string(mysql_error(conn_)));
        }

        MYSQL_RES* r = mysql_store_result(conn_);
        report_query(cfg_, sql, start);
        if (!r && mysql_field_count(conn_) != 0)
            throw error("cboot.orm: " + std::string(mysql_error(conn_)));
        if (r) mysql_free_result(r);
    }

    std::int64_t last_insert_id() override {
        ensure_thread();
        return static_cast<std::int64_t>(mysql_insert_id(conn_));
    }

    /**
     * @brief 真实探测（一次往返）：mysql_ping 校验链路是否存活
     *
     * libmysqlclient ≥5.7 默认关闭自动重连，ping 失败即链路已死；
     * 死连接交还给池方重建。
     */
    bool probe() {
        ensure_thread();
        return mysql_ping(conn_) == 0;
    }

    /** @brief 是否需要借出前探测：空闲超过 idle_probe_s 且开关打开 */
    bool probe_due() const {
        if (!cfg_ || cfg_->idle_probe_s <= 0) return false;
        const auto idle = std::chrono::steady_clock::now() - last_used_;
        return idle > std::chrono::seconds(cfg_->idle_probe_s);
    }

    /** @brief 记录最近使用时间（借出/归还时调用） */
    void touch() { last_used_ = std::chrono::steady_clock::now(); }

    MysqlProps* cfg_ = nullptr;

private:
    MYSQL* conn_;
    std::chrono::steady_clock::time_point last_used_{
        std::chrono::steady_clock::now()};
};

} // namespace

// ==================== MysqlDataSource::impl ====================

struct MysqlDataSource::impl {
    MysqlProps* cfg = nullptr;
    health_registry* health = nullptr;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::unique_ptr<mysql_connection>> idle;
    std::size_t total = 0;
    bool closed = false;

    /**
     * @brief 建立一条 MySQL 连接；失败抛 orm::error
     *
     * password 为空或未展开的 ${MYSQL_PWD} 时回退 MYSQL_PWD；
     * 错误信息来自 mysql_error，不包含密码。
     */
    std::unique_ptr<mysql_connection> connect_one() {
        ensure_thread();

        MYSQL* conn = mysql_init(nullptr);
        if (!conn) throw error("cboot.orm: mysql_init failed");

        const unsigned int timeout = static_cast<unsigned int>(
            cfg->connect_timeout_s > 0 ? cfg->connect_timeout_s : 5);
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (!cfg->charset.empty())
            mysql_options(conn, MYSQL_SET_CHARSET_NAME, cfg->charset.c_str());

        std::string password = cfg->password;
        if (password.empty() || password.rfind("${", 0) == 0) {
            const char* env = std::getenv("MYSQL_PWD");
            password = env ? env : "";
        }

        const std::string host = cfg->host.empty() ? "127.0.0.1" : cfg->host;
        const std::string user = cfg->user.empty() ? "root" : cfg->user;
        const unsigned int port = static_cast<unsigned int>(
            cfg->port > 0 ? cfg->port : 3306);
        const char* db = cfg->database.empty() ? nullptr : cfg->database.c_str();
        const char* pass = password.empty() ? nullptr : password.c_str();

        MYSQL* ok = mysql_real_connect(conn, host.c_str(), user.c_str(), pass,
                                       db, port, nullptr, 0);
        if (!ok) {
            std::string msg = mysql_error(conn);
            mysql_close(conn);
            if (health) health->set_unhealthy("mysql: " + msg);
            throw error("cboot.orm: connection failed: " + msg);
        }
        if (!cfg->charset.empty()
            && mysql_set_character_set(conn, cfg->charset.c_str()) != 0) {
            std::string msg = mysql_error(conn);
            mysql_close(conn);
            if (health) health->set_unhealthy("mysql: " + msg);
            throw error("cboot.orm: set charset failed: " + msg);
        }

        if (health) health->set_healthy();
        return std::make_unique<mysql_connection>(cfg, conn);
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
    void release(mysql_connection* c) {
        std::unique_ptr<mysql_connection> p(c);
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
     * @brief 借出前校验：空闲过久时一次 mysql_ping 探测
     *
     * 校验失败 → 销毁死连接并重建（保持池容量不变，业务无感知）；
     * 重建计入 orm_pool_reconnects_total。重建失败按连接失败处理。
     */
    std::unique_ptr<mysql_connection> validate_or_reconnect(
        std::unique_ptr<mysql_connection> c) {
        if (!c->probe_due() || c->probe()) return c;
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
                    release(static_cast<mysql_connection*>(p));
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
                        release(static_cast<mysql_connection*>(p));
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

// ==================== MysqlDataSource ====================

MysqlDataSource::MysqlDataSource(MysqlProps& cfg, health_registry& health)
    : impl_(std::make_unique<impl>()) {
    impl_->cfg = &cfg;
    impl_->health = &health;
}

MysqlDataSource::~MysqlDataSource() {
    close();
}

void MysqlDataSource::open() {
    impl_->open();
}

void MysqlDataSource::close() {
    impl_->close();
}

connection_lease MysqlDataSource::acquire() {
    return impl_->acquire();
}

} // namespace orm
