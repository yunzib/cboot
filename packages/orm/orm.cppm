/**
 * @file orm.cppm
 * @brief ORM 通用组件公共入口
 *
 * 重新导出实体注解、驱动接口、元数据和 DSL；
 * 本模块实现 EntityManager、事务 session 与通用 orm_component。
 *
 * 具体数据库由独立驱动组件提供：
 *   - cboot.orm.postgres → postgres_component
 *   - cboot.orm.mysql    → mysql_component
 * 应用选择驱动组件后，依赖闭包自动带入 orm_component。
 *
 * 用户通常只 import cboot.orm 以及所选驱动模块。
 */
export module cboot.orm;

import std;
import cboot.core.annotations;   // Bean / bean_name
import cboot.meta.reflect;       // type_list
import cboot.orm.annotations;
import cboot.orm.driver;
import cboot.orm.meta;
import cboot.orm.dsl;

// 单一入口：import cboot.orm 即获得全部通用 ORM 词汇。
export import cboot.orm.annotations;
export import cboot.orm.driver;
export import cboot.orm.meta;
export import cboot.orm.dsl;

export namespace orm {

namespace detail {

// ── 连接级操作：EntityManager 与 session 共用 ─────────────────────────

template <class T>
std::optional<T> find_impl(connection& conn, const dialect& d,
                           const id_type<T>& id) {
    auto stmt = conn.prepare(select_by_id_sql<T>(d));
    stmt->bind(0, to_db_value(id));
    if (!stmt->next()) return std::nullopt;
    return read_row<T>(*stmt);
}

template <class T>
std::vector<T> all_impl(connection& conn, const dialect& d) {
    auto stmt = conn.prepare(select_all_sql<T>(d));
    std::vector<T> out;
    while (stmt->next()) out.push_back(read_row<T>(*stmt));
    return out;
}

template <class T>
void insert_impl(connection& conn, const dialect& d, T& entity) {
    const auto& meta = entity_meta_v<T>;
    auto stmt = conn.prepare(insert_sql<T>(d));
    auto values = insert_values(entity);
    for (std::size_t i = 0; i < values.size(); ++i)
        stmt->bind(i, values[i]);

    if (!meta.id_generated) {
        (void)stmt->next();
        return;
    }

    auto& id = entity.[:nth_member<T, meta.id_index>():];
    if (d.supports_returning()) {
        if (!stmt->next())
            throw error("cboot.orm: INSERT RETURNING produced no row");
        read_scalar(*stmt, 0, id);
        return;
    }

    (void)stmt->next();
    const std::int64_t generated = conn.last_insert_id();
    id = static_cast<std::remove_cvref_t<decltype(id)>>(generated);
}

template <class T>
std::size_t update_impl(connection& conn, const dialect& d, const T& entity) {
    auto stmt = conn.prepare(update_sql<T>(d));
    auto values = update_values(entity);
    for (std::size_t i = 0; i < values.size(); ++i)
        stmt->bind(i, values[i]);
    (void)stmt->next();
    return stmt->affected_rows();
}

template <class T>
std::size_t remove_impl(connection& conn, const dialect& d,
                        const id_type<T>& id) {
    auto stmt = conn.prepare(delete_sql<T>(d));
    stmt->bind(0, to_db_value(id));
    (void)stmt->next();
    return stmt->affected_rows();
}

template <class T>
std::vector<T> fetch_rows_impl(connection& conn, std::string_view sql,
                               const std::vector<db_value>& params) {
    auto stmt = conn.prepare(sql);
    for (std::size_t i = 0; i < params.size(); ++i)
        stmt->bind(i, params[i]);
    std::vector<T> out;
    while (stmt->next()) out.push_back(read_row<T>(*stmt));
    return out;
}

template <class T>
std::vector<T> query_impl(connection& conn, std::string_view sql,
                          const std::vector<db_value>& params) {
    auto stmt = conn.prepare(sql);
    for (std::size_t i = 0; i < params.size(); ++i)
        stmt->bind(i, params[i]);
    return fetch_rows_named<T>(*stmt);
}

inline std::size_t execute_impl(connection& conn, std::string_view sql,
                                const std::vector<db_value>& params) {
    auto stmt = conn.prepare(sql);
    for (std::size_t i = 0; i < params.size(); ++i)
        stmt->bind(i, params[i]);
    (void)stmt->next();
    return stmt->affected_rows();
}

template <class T>
void create_schema_impl(connection& conn, const dialect& d) {
    conn.execute(create_table_sql<T>(d));
}

template <class T>
void drop_schema_impl(connection& conn, const dialect& d) {
    conn.execute(drop_table_sql<T>(d));
}

} // namespace detail

/**
 * @brief 固定单连接的 ORM 事务会话
 *
 * session 持有连接租借，事务内的 CRUD/DSL/原生 SQL 共用同一连接。
 * 正常路径由 EntityManager::transaction 提交；析构未完成时回滚。
 * session 不是线程安全的，也不支持嵌套。
 */
class session {
public:
    session(connection_lease lease, const dialect& d)
        : lease_(std::move(lease)), dialect_(&d) {}
    ~session() {
        if (!finished_) rollback_noexcept();
    }

    session(const session&) = delete;
    session& operator=(const session&) = delete;
    session(session&&) noexcept = default;
    session& operator=(session&&) noexcept = delete;

    /** @brief 当前事务方言 */
    const dialect& dialect() const { return *dialect_; }

    /** @brief 提交；已结束则抛错 */
    void commit() {
        if (finished_) throw error("cboot.orm: transaction already finished");
        lease_->execute("COMMIT");
        finished_ = true;
    }

    /** @brief 回滚；已结束则忽略 */
    void rollback() {
        if (finished_) return;
        lease_->execute("ROLLBACK");
        finished_ = true;
    }

    /** @brief 事务是否已提交/回滚（用于 transaction 避免重复 COMMIT） */
    bool finished() const { return finished_; }

    template <class T>
    std::optional<T> find(const id_type<T>& id) {
        return detail::find_impl<T>(*lease_, *dialect_, id);
    }

    template <class T>
    std::vector<T> all() {
        return detail::all_impl<T>(*lease_, *dialect_);
    }

    template <class T>
    void insert(T& entity) {
        detail::insert_impl<T>(*lease_, *dialect_, entity);
    }

    template <class T>
    std::size_t update(const T& entity) {
        return detail::update_impl<T>(*lease_, *dialect_, entity);
    }

    template <class T>
    std::size_t remove(const id_type<T>& id) {
        return detail::remove_impl<T>(*lease_, *dialect_, id);
    }

    template <class T>
    void create_schema() {
        detail::create_schema_impl<T>(*lease_, *dialect_);
    }

    template <class T>
    void drop_schema() {
        detail::drop_schema_impl<T>(*lease_, *dialect_);
    }

    /** @brief 低层查询：按 SELECT 列顺序映射实体 */
    template <class T>
    std::vector<T> fetch_rows(std::string_view sql,
                              const std::vector<db_value>& params) {
        return detail::fetch_rows_impl<T>(*lease_, sql, params);
    }

    /** @brief 原生 SQL：按结果列名映射实体（支持 alias/join） */
    template <class T>
    std::vector<T> query(std::string_view sql,
                         const std::vector<db_value>& params = {}) {
        return detail::query_impl<T>(*lease_, sql, params);
    }

    /** @brief 低层执行：DML/DDL，返回影响行数 */
    std::size_t execute(std::string_view sql,
                        const std::vector<db_value>& params) {
        return detail::execute_impl(*lease_, sql, params);
    }

    template <class T>
    select_builder<T, session> select() {
        return select_builder<T, session>{*this};
    }

    template <class T>
    update_builder<T, session> update() {
        return update_builder<T, session>{*this};
    }

    template <class T>
    delete_builder<T, session> remove() {
        return delete_builder<T, session>{*this};
    }

private:
    void rollback_noexcept() noexcept {
        try {
            if (lease_) lease_->execute("ROLLBACK");
        } catch (...) {
            // 析构不能抛；连接归还池后由池/数据库回收状态
        }
        finished_ = true;
    }

    connection_lease lease_;
    const orm::dialect* dialect_ = nullptr;
    bool finished_ = false;
};

/**
 * @brief 实体管理器
 *
 * 提供：
 *   - 简单 CRUD：find / all / insert / update / remove
 *   - 建表/删表：create_schema / drop_schema（开发/测试）
 *   - 类型安全 DSL 入口：select / update / remove
 *   - 低层执行：fetch_rows / query / execute
 *   - 事务：transaction(callback)
 *
 * 非事务操作每次从 DataSource 租借连接；事务内使用 session 固定连接。
 */
class [[=Bean{}]] EntityManager {
public:
    /** @brief 绑定数据源；EntityManager 不拥有连接池 */
    explicit EntityManager(DataSource& ds) : ds_(ds) {}

    /** @brief 当前数据源的 SQL 方言；DSL builder 用于渲染 SQL */
    const dialect& dialect() const { return ds_.dialect(); }

    /**
     * @brief 在单连接事务中执行 callback
     *
     * 正常返回 → COMMIT；抛异常 → ROLLBACK 后重新抛出。
     * callback 内的 CRUD/DSL/原生 SQL 必须使用传入的 session，
     * 不要再用 EntityManager 借新连接（会造成事务外的第二次操作）。
     */
    template <class F>
    decltype(auto) transaction(F&& fn) {
        auto lease = ds_.acquire();
        lease->execute("BEGIN");
        session tx(std::move(lease), dialect());
        using R = std::invoke_result_t<F, session&>;
        if constexpr (std::is_void_v<R>) {
            std::invoke(std::forward<F>(fn), tx);
            if (!tx.finished()) tx.commit();
        } else {
            R result = std::invoke(std::forward<F>(fn), tx);
            if (!tx.finished()) tx.commit();
            return result;
        }
    }

    /** @brief 按主键查询；未命中返回 nullopt */
    template <class T>
    std::optional<T> find(const id_type<T>& id) {
        auto lease = ds_.acquire();
        return detail::find_impl<T>(*lease, dialect(), id);
    }

    /** @brief 查询全表 */
    template <class T>
    std::vector<T> all() {
        auto lease = ds_.acquire();
        return detail::all_impl<T>(*lease, dialect());
    }

    /** @brief 插入实体；自增 id 按方言回填 */
    template <class T>
    void insert(T& entity) {
        auto lease = ds_.acquire();
        detail::insert_impl<T>(*lease, dialect(), entity);
    }

    /** @brief 按主键更新；返回影响行数 */
    template <class T>
    std::size_t update(const T& entity) {
        auto lease = ds_.acquire();
        return detail::update_impl<T>(*lease, dialect(), entity);
    }

    /** @brief 按主键删除；返回影响行数 */
    template <class T>
    std::size_t remove(const id_type<T>& id) {
        auto lease = ds_.acquire();
        return detail::remove_impl<T>(*lease, dialect(), id);
    }

    template <class T>
    void create_schema() {
        auto lease = ds_.acquire();
        detail::create_schema_impl<T>(*lease, dialect());
    }

    template <class T>
    void drop_schema() {
        auto lease = ds_.acquire();
        detail::drop_schema_impl<T>(*lease, dialect());
    }

    /**
     * @brief 低层查询：按 SELECT 列顺序映射实体
     *
     * 给 DSL 和原生 SQL 逃生口使用；参数顺序必须与 SQL 占位符对应。
     */
    template <class T>
    std::vector<T> fetch_rows(std::string_view sql,
                              const std::vector<db_value>& params) {
        auto lease = ds_.acquire();
        return detail::fetch_rows_impl<T>(*lease, sql, params);
    }

    /**
     * @brief 原生 SQL：按结果列名映射实体（支持 alias/join）
     *
     * 列名与实体 Column 元数据匹配；缺列抛 orm::error。
     */
    template <class T>
    std::vector<T> query(std::string_view sql,
                         const std::vector<db_value>& params = {}) {
        auto lease = ds_.acquire();
        return detail::query_impl<T>(*lease, sql, params);
    }

    /** @brief 低层执行：DML/DDL，返回影响行数 */
    std::size_t execute(std::string_view sql,
                        const std::vector<db_value>& params) {
        auto lease = ds_.acquire();
        return detail::execute_impl(*lease, sql, params);
    }

    /** @brief DSL：单表 SELECT */
    template <class T>
    select_builder<T, EntityManager> select() {
        return select_builder<T, EntityManager>{*this};
    }

    /** @brief DSL：条件 UPDATE */
    template <class T>
    update_builder<T, EntityManager> update() {
        return update_builder<T, EntityManager>{*this};
    }

    /** @brief DSL：条件 DELETE */
    template <class T>
    delete_builder<T, EntityManager> remove() {
        return delete_builder<T, EntityManager>{*this};
    }

private:
    DataSource& ds_;
};

} // namespace orm

/**
 * @brief ORM 通用契约组件描述符
 *
 * 只贡献 EntityManager；具体 DataSource 必须由驱动组件提供。
 */
export struct orm_component {
    static constexpr bean_name name{"orm"};
    using beans = type_list<orm::EntityManager>;
    using dependencies = type_list<>;
};
