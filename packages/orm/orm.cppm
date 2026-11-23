/**
 * @file orm.cppm
 * @brief ORM 通用组件公共入口
 *
 * 重新导出实体注解、驱动接口、元数据和 DSL；
 * 本模块只实现 EntityManager 与通用 orm_component。
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

/**
 * @brief 实体管理器
 *
 * 提供：
 *   - 简单 CRUD：find / all / insert / update / remove
 *   - 建表/删表：create_schema / drop_schema（开发/测试）
 *   - 类型安全 DSL 入口：select / update / remove
 *   - 低层执行：fetch_rows / execute（DSL 与原生 SQL 用）
 *
 * 所有 SQL 都按 DataSource::dialect() 生成，因此同一个 EntityManager
 * 可以运行在 PostgreSQL / MySQL 等驱动之上。
 *
 * 第一版同步调用，每次操作从 DataSource 租借连接；
 * 事务 API 在后续阶段接入。
 */
class [[=Bean{}]] EntityManager {
public:
    /** @brief 绑定数据源；EntityManager 不拥有连接池 */
    explicit EntityManager(DataSource& ds) : ds_(ds) {}

    /** @brief 当前数据源的 SQL 方言；DSL builder 用于渲染 SQL */
    const dialect& dialect() const { return ds_.dialect(); }

    /**
     * @brief 按主键查询
     *
     * SQL 由 dialect 生成（占位符/标识符引用与数据库一致）。
     * 未命中返回 nullopt。
     */
    template <class T>
    std::optional<T> find(const id_type<T>& id) {
        const auto& d = dialect();
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(select_by_id_sql<T>(d));
        stmt->bind(0, detail::to_db_value(id));
        if (!stmt->next()) return std::nullopt;
        return read_row<T>(*stmt);
    }

    /** @brief 查询全表；第一版不提供分页，用 DSL 做 limit/offset */
    template <class T>
    std::vector<T> all() {
        const auto& d = dialect();
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(select_all_sql<T>(d));
        std::vector<T> out;
        while (stmt->next()) out.push_back(read_row<T>(*stmt));
        return out;
    }

    /**
     * @brief 插入实体
     *
     * 自增主键：
     *   - 支持 RETURNING 的方言（PostgreSQL）：INSERT ... RETURNING id；
     *   - 不支持的方言（MySQL）：INSERT 后读取 connection::last_insert_id()。
     *
     * 非自增主键：id 由调用方提供，参与 INSERT。
     */
    template <class T>
    void insert(T& entity) {
        const auto& d = dialect();
        const auto& meta = entity_meta_v<T>;
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(insert_sql<T>(d));
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
            detail::read_scalar(*stmt, 0, id);
            return;
        }

        (void)stmt->next();
        const std::int64_t generated = lease->last_insert_id();
        id = static_cast<std::remove_cvref_t<decltype(id)>>(generated);
    }

    /**
     * @brief 按主键更新实体
     *
     * 只更新非 id 列；WHERE 使用 id。
     * 返回影响行数（0 表示主键不存在）。
     */
    template <class T>
    std::size_t update(const T& entity) {
        const auto& d = dialect();
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(update_sql<T>(d));
        auto values = update_values(entity);
        for (std::size_t i = 0; i < values.size(); ++i)
            stmt->bind(i, values[i]);
        (void)stmt->next();
        return stmt->affected_rows();
    }

    /** @brief 按主键删除；返回影响行数 */
    template <class T>
    std::size_t remove(const id_type<T>& id) {
        const auto& d = dialect();
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(delete_sql<T>(d));
        stmt->bind(0, detail::to_db_value(id));
        (void)stmt->next();
        return stmt->affected_rows();
    }

    /** @brief 按实体元数据建表（IF NOT EXISTS，开发/测试用） */
    template <class T>
    void create_schema() {
        const auto& d = dialect();
        auto lease = ds_.acquire();
        lease->execute(create_table_sql<T>(d));
    }

    /** @brief 删除实体表（IF EXISTS，开发/测试用） */
    template <class T>
    void drop_schema() {
        const auto& d = dialect();
        auto lease = ds_.acquire();
        lease->execute(drop_table_sql<T>(d));
    }

    /**
     * @brief 低层查询：执行 SQL 并按实体映射行
     *
     * 给 DSL 和原生 SQL 逃生口使用；参数顺序必须与 SQL 占位符对应。
     */
    template <class T>
    std::vector<T> fetch_rows(std::string_view sql,
                              const std::vector<db_value>& params) {
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(sql);
        for (std::size_t i = 0; i < params.size(); ++i)
            stmt->bind(i, params[i]);
        std::vector<T> out;
        while (stmt->next()) out.push_back(read_row<T>(*stmt));
        return out;
    }

    /**
     * @brief 低层执行：DML/DDL，返回影响行数
     *
     * 给 DSL 和原生 SQL 逃生口使用；不负责事务。
     */
    std::size_t execute(std::string_view sql,
                        const std::vector<db_value>& params) {
        auto lease = ds_.acquire();
        auto stmt = lease->prepare(sql);
        for (std::size_t i = 0; i < params.size(); ++i)
            stmt->bind(i, params[i]);
        (void)stmt->next();
        return stmt->affected_rows();
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
 * 只贡献 EntityManager；具体 DataSource 必须由驱动组件提供：
 *   applications: components = type_list<postgres_component>;
 * 或 components = type_list<mysql_component>;
 */
export struct orm_component {
    static constexpr bean_name name{"orm"};
    using beans = type_list<orm::EntityManager>;
    using dependencies = type_list<>;
};
