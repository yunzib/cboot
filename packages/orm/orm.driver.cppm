/**
 * @file orm.driver.cppm
 * @brief ORM 驱动接口与 SQL 方言抽象
 *
 * JDBC 风格的 connection / statement / DataSource 抽象，外加 dialect：
 *   - dialect 只描述标识符引用、占位符、类型名和方言能力；
 *   - EntityManager / DSL 通过 dialect 生成可移植 SQL；
 *   - 具体数据库驱动实现 DataSource，业务代码不接触驱动类型。
 */
export module cboot.orm.driver;

import std;
import cboot.orm.annotations;   // error

/**
 * @brief ORM 驱动接口（JDBC 风格，但只给驱动/低层执行使用）
 *
 * 用户侧应使用 EntityManager / DSL，不直接接触这些类型。
 */

export namespace orm {

/** @brief 参数值；nullptr 表示 SQL NULL */
using db_value =
    std::variant<std::nullptr_t, std::int64_t, double, std::string, bool>;

/** @brief 参数值 → 通用文本绑定；nullopt 表示 NULL */
inline std::optional<std::string> to_bind_text(const db_value& v) {
    if (std::holds_alternative<std::nullptr_t>(v)) return std::nullopt;
    if (auto p = std::get_if<bool>(&v)) return *p ? "true" : "false";
    if (auto p = std::get_if<std::int64_t>(&v)) return std::to_string(*p);
    if (auto p = std::get_if<double>(&v)) {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, *p);
        return std::string(buf, r.ptr);
    }
    if (auto p = std::get_if<std::string>(&v)) return *p;
    return std::nullopt;
}

/**
 * @brief 方言无关的 SQL 标量类型
 *
 * 实体成员类型先映射到这些逻辑类型，再由 dialect 转成具体数据库类型。
 * 这样 MySQL / PostgreSQL 的类型差异不会泄漏到实体元数据。
 */
enum class sql_type {
    boolean,
    integer,
    big_integer,
    real,
    text,
};

/**
 * @brief SQL 方言
 *
 * 只包含 ORM 生成 SQL 所需的最小能力集；驱动可以实现自己的扩展，
 * 但 EntityManager/DSL 只依赖这里声明的方法。
 *
 * 生命周期：由 DataSource 持有的静态对象或成员对象，引用在 DataSource
 * 生命周期内有效。
 */
class dialect {
public:
    virtual ~dialect() = default;

    /** @brief 引用标识符（表名/列名） */
    virtual std::string quote_identifier(std::string_view name) const = 0;

    /** @brief 生成第 index 个占位符（1 基） */
    virtual std::string placeholder(std::size_t index) const = 0;

    /** @brief 逻辑类型 → 数据库列类型 */
    virtual std::string type_name(sql_type type) const = 0;

    /** @brief 自增主键列的完整类型声明 */
    virtual std::string identity_type(sql_type type) const = 0;

    /**
     * @brief 无普通列时的 INSERT 语句
     *
     * PostgreSQL 是 INSERT ... DEFAULT VALUES；MySQL 是 INSERT ... () VALUES ()。
     * 参数 quoted_table 已由 quote_identifier 处理。
     */
    virtual std::string insert_default_values(std::string_view quoted_table) const = 0;

    /** @brief INSERT 是否支持 RETURNING 回填自增主键 */
    virtual bool supports_returning() const = 0;

    /** @brief 是否支持 ILIKE 大小写不敏感匹配 */
    virtual bool supports_ilike() const = 0;
};

class statement {
public:
    virtual ~statement() = default;

    /** @brief 绑定第 index 个参数（0 基） */
    virtual void bind(std::size_t index, const db_value& value) = 0;

    /** @brief 执行并前进一行；无结果集返回 false */
    virtual bool next() = 0;

    virtual bool is_null(std::size_t column) = 0;
    virtual std::int64_t get_int(std::size_t column) = 0;
    virtual double get_double(std::size_t column) = 0;
    virtual std::string_view get_text(std::size_t column) = 0;

    /** @brief 结果集列数（无结果集返回 0） */
    virtual std::size_t column_count() const = 0;

    /** @brief 第 column 列的名字；无结果集返回空 */
    virtual std::string_view column_name(std::size_t column) const = 0;

    /** @brief DML 影响行数（无则 0） */
    virtual std::size_t affected_rows() const = 0;
};

class connection {
public:
    virtual ~connection() = default;
    virtual std::unique_ptr<statement> prepare(std::string_view sql) = 0;
    virtual void execute(std::string_view sql) = 0;

    /**
     * @brief 最近一次 INSERT 生成的自增主键
     *
     * 不支持 RETURNING 的方言在 INSERT 后调用；没有自增主键时返回 0。
     */
    virtual std::int64_t last_insert_id() = 0;
};

/** @brief connection 的 RAII 租借；归还由连接池负责 */
class connection_lease {
public:
    connection_lease() = default;
    connection_lease(connection* c, std::function<void(connection*)> release)
        : conn_(c), release_(std::move(release)) {}
    ~connection_lease() {
        if (conn_ && release_) release_(conn_);
    }
    connection_lease(connection_lease&& other) noexcept
        : conn_(std::exchange(other.conn_, nullptr)),
          release_(std::move(other.release_)) {}
    connection_lease& operator=(connection_lease&& other) noexcept {
        if (this == &other) return *this;
        if (conn_ && release_) release_(conn_);
        conn_ = std::exchange(other.conn_, nullptr);
        release_ = std::move(other.release_);
        return *this;
    }
    connection_lease(const connection_lease&) = delete;
    connection_lease& operator=(const connection_lease&) = delete;

    connection* operator->() const { return conn_; }
    connection& operator*() const { return *conn_; }
    explicit operator bool() const { return conn_ != nullptr; }

private:
    connection* conn_ = nullptr;
    std::function<void(connection*)> release_;
};

class DataSource {
public:
    virtual ~DataSource() = default;
    virtual connection_lease acquire() = 0;

    /** @brief 本数据源使用的 SQL 方言；引用在 DataSource 生命周期内有效 */
    virtual const dialect& dialect() const = 0;
};

} // namespace orm
