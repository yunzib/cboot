/**
 * @file orm.mysql.cppm
 * @brief MySQL 驱动契约组件
 *
 * 本组件只提供 MySQL 特有部分：
 *   - MysqlProps：连接配置（前缀 db）
 *   - MysqlDialect：反引号、? 占位符、AUTO_INCREMENT、TINYINT(1)
 *   - MysqlDataSource：libmysql 连接池
 *   - mysql_component：依赖通用 orm_component
 *
 * mysql.h 不进入模块接口；连接池和语句细节在 src/mysql.cpp。
 */
export module cboot.orm.mysql;

import std;
import cboot.core.annotations;   // Bean / PostConstruct / PreDestroy / Config
import cboot.core.context;       // health_registry
import cboot.meta.reflect;       // type_list
export import cboot.orm;         // 通用 ORM 词汇（含 dialect / DataSource）
import cboot.orm.driver;

export namespace orm {

/**
 * @brief MySQL 连接配置（前缀 db）
 *
 * password：
 *   - 留空：回退 MYSQL_PWD 环境变量
 *   - 写 "${MYSQL_PWD}"：由 config 的 ${ENV} 展开；未展开时同样回退环境变量
 *   - 直接写值：使用该值
 *
 * 密码不会出现在错误信息/日志里。
 */
struct [[=Config{"db"}]] MysqlProps {
    std::string host{"127.0.0.1"};
    int port{3306};
    std::string database{"mysql"};
    std::string user{"root"};
    std::string password;
    std::string charset{"utf8mb4"};
    int pool_size{4};
    int connect_timeout_s{5};
    int acquire_timeout_ms{5000};
};

/**
 * @brief MySQL SQL 方言
 *
 * 引用：反引号；占位符：?；自增：AUTO_INCREMENT；
 * 不支持 RETURNING，自增主键由 mysql_insert_id 回填。
 */
class MysqlDialect final : public dialect {
public:
    std::string quote_identifier(std::string_view name) const override {
        std::string out;
        out.reserve(name.size() + 2);
        out.push_back('`');
        for (char c : name) {
            if (c == '`') out += "``";
            else out.push_back(c);
        }
        out.push_back('`');
        return out;
    }

    std::string placeholder(std::size_t) const override { return "?"; }

    std::string type_name(sql_type type) const override {
        switch (type) {
        case sql_type::boolean: return "TINYINT(1)";
        case sql_type::integer: return "INT";
        case sql_type::big_integer: return "BIGINT";
        case sql_type::real: return "DOUBLE";
        case sql_type::text: return "TEXT";
        }
        return "TEXT";
    }

    std::string identity_type(sql_type type) const override {
        return type_name(type) + " AUTO_INCREMENT";
    }

    std::string insert_default_values(std::string_view table) const override {
        return "INSERT INTO " + std::string(table) + " () VALUES ()";
    }

    bool supports_returning() const override { return false; }
    bool supports_ilike() const override { return false; }
};

/** @brief 进程级 MySQL 方言单例；MysqlDataSource 返回其引用 */
inline const dialect& mysql_dialect() {
    static const MysqlDialect d;
    return d;
}

/**
 * @brief MySQL 连接池 DataSource
 *
 * 公开接口只暴露 DataSource 语义；mysql.h 不进入模块接口。
 * 运行时需要 libmysql.dll 在 PATH 或 exe 同目录。
 */
class [[=Bean{}]] MysqlDataSource : public DataSource {
public:
    /** @brief 构造但不立即连接 */
    MysqlDataSource(MysqlProps& cfg, health_registry& health);
    ~MysqlDataSource() override;

    /** @brief 建立连接池；失败抛 orm::error 并标记 health DOWN */
    [[=PostConstruct{}]] void open();

    /** @brief 关闭空闲连接；忙连接归还时销毁 */
    [[=PreDestroy{}]] void close();

    /** @brief 从池中租借一个连接；池满时按 acquire_timeout_ms 等待 */
    connection_lease acquire() override;

    /** @brief MySQL 方言 */
    const orm::dialect& dialect() const override { return mysql_dialect(); }

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace orm

/**
 * @brief MySQL 驱动契约组件描述符
 *
 * 选中后贡献 MysqlProps / MysqlDataSource，并通过依赖闭包带入
 * orm_component（EntityManager）。应用只需选择本组件。
 */
export struct mysql_component {
    static constexpr bean_name name{"orm.mysql"};
    using beans = type_list<orm::MysqlProps, orm::MysqlDataSource>;
    using dependencies = type_list<orm_component>;
};
