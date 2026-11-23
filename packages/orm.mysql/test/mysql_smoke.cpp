/**
 * @file mysql_smoke.cpp
 * @brief MySQL 驱动冒烟测试
 *
 * 覆盖：
 *   - MysqlDialect 的 CRUD/DDL SQL（反引号、?、AUTO_INCREMENT、无 RETURNING）
 *   - mysql_component 依赖闭包（MysqlProps + MysqlDataSource + EntityManager）
 *   - 可选 MySQL 集成：CBOOT_TEST_MYSQL_DATABASE 存在时执行真实 CRUD
 *
 * 集成测试所需环境变量：
 *   CBOOT_TEST_MYSQL_HOST（默认 127.0.0.1）
 *   CBOOT_TEST_MYSQL_PORT（默认 3306）
 *   CBOOT_TEST_MYSQL_DATABASE（必填）
 *   CBOOT_TEST_MYSQL_USER（默认 root）
 *   CBOOT_TEST_MYSQL_PASSWORD（可选；也可用 MYSQL_PWD）
 */
import cboot.app;               // application / app_beans_t（组件描述符校验）
import cboot.orm.mysql;
import cboot.core.context;      // health_registry
import std;

struct [[=orm::Table{.name = "cboot_orm_mysql_smoke_users"}]] MysqlUser {
    [[=orm::Id{.generated = true}]] std::int64_t id{};
    [[=orm::Column{"display_name"}]] std::string name;
    std::optional<std::string> email;
    bool active{};
};

struct [[=orm::Table{.name = "cboot_orm_mysql_smoke_ids"}]] MysqlIdOnly {
    [[=orm::Id{.generated = true}]] std::int64_t id{};
};

/** @brief 类型清单包含判断（测试用） */
template <class L, class T>
struct contains;
template <class... Ts, class T>
struct contains<type_list<Ts...>, T>
    : std::bool_constant<(std::is_same_v<Ts, T> || ...)> {};

/** @brief mysql_component 依赖闭包带入通用 ORM bean */
struct MysqlApp : application<MysqlApp> {
    using components = type_list<mysql_component>;
};
static_assert(contains<app_beans_t<MysqlApp>, orm::MysqlProps>::value);
static_assert(contains<app_beans_t<MysqlApp>, orm::MysqlDataSource>::value);
static_assert(contains<app_beans_t<MysqlApp>, orm::EntityManager>::value);

int main() {
    const auto& d = orm::mysql_dialect();

    // ── 1. 元数据 ──────────────────────────────────────────────────────
    static_assert(orm::entity_meta_v<MysqlUser>.id_index == 0);
    static_assert(orm::entity_meta_v<MysqlUser>.id_generated);
    static_assert(orm::entity_meta_v<MysqlUser>.columns.size() == 4);

    // ── 2. SQL 生成 ────────────────────────────────────────────────────
    const std::string expected_select =
        R"(SELECT `id`,`display_name`,`email`,`active` FROM `cboot_orm_mysql_smoke_users`)";
    if (orm::select_all_sql<MysqlUser>(d) != expected_select) return 1;

    const std::string expected_insert =
        R"(INSERT INTO `cboot_orm_mysql_smoke_users` (`display_name`,`email`,`active`) VALUES (?,?,?))";
    if (orm::insert_sql<MysqlUser>(d) != expected_insert) return 2;

    const std::string expected_default_insert =
        R"(INSERT INTO `cboot_orm_mysql_smoke_ids` () VALUES ())";
    if (orm::insert_sql<MysqlIdOnly>(d) != expected_default_insert) return 24;

    const std::string expected_update =
        R"(UPDATE `cboot_orm_mysql_smoke_users` SET `display_name` = ?, `email` = ?, `active` = ? WHERE `id` = ?)";
    if (orm::update_sql<MysqlUser>(d) != expected_update) return 3;

    const std::string expected_delete =
        R"(DELETE FROM `cboot_orm_mysql_smoke_users` WHERE `id` = ?)";
    if (orm::delete_sql<MysqlUser>(d) != expected_delete) return 4;

    const std::string ddl = orm::create_table_sql<MysqlUser>(d);
    if (ddl.find(R"(`id` BIGINT AUTO_INCREMENT PRIMARY KEY)")
        == std::string::npos)
        return 5;
    if (ddl.find(R"(`email` TEXT)") == std::string::npos) return 6;
    if (ddl.find(R"(`active` TINYINT(1))") == std::string::npos) return 7;

    // ── 3. DSL 条件 / 排序 ─────────────────────────────────────────────
    auto cond = orm::col<^^MysqlUser::active>() == true
        && orm::col<^^MysqlUser::name>() == "bob";
    auto rendered = cond.render(d);
    if (rendered.sql != R"((`active` = ?) AND (`display_name` = ?))")
        return 8;
    if (rendered.params.size() != 2) return 9;

    auto order = orm::desc(orm::col<^^MysqlUser::id>());
    if (order.column != "id" || order.ascending) return 10;

    auto null_cond = orm::is_null(orm::col<^^MysqlUser::email>());
    if (null_cond.render(d).sql != R"(`email` IS NULL)") return 11;

    auto in_cond = orm::in(orm::col<^^MysqlUser::id>(),
                           std::vector<std::int64_t>{1, 2});
    auto in_sql = in_cond.render(d);
    if (in_sql.sql != R"(`id` IN (?,?))" || in_sql.params.size() != 2)
        return 12;

    auto empty_in = orm::in(orm::col<^^MysqlUser::id>(),
                            std::vector<std::int64_t>{});
    if (empty_in.render(d).sql != "FALSE"
        || !empty_in.render(d).params.empty())
        return 13;

    auto like_cond = orm::like(orm::col<^^MysqlUser::name>(), "bob%");
    auto like_sql = like_cond.render(d);
    if (like_sql.sql != R"(`display_name` LIKE ?)"
        || like_sql.params.size() != 1)
        return 14;

    // MySQL 没有 ILIKE，DSL 降级为 LOWER(...) LIKE LOWER(?)
    auto ilike_cond = orm::ilike(orm::col<^^MysqlUser::name>(), "bob%");
    if (ilike_cond.render(d).sql
        != R"(LOWER(`display_name`) LIKE LOWER(?))")
        return 15;

    // ── 4. 可选集成测试 ────────────────────────────────────────────────
    const char* database = std::getenv("CBOOT_TEST_MYSQL_DATABASE");
    if (!database || !*database) {
        std::println("ORM MYSQL SMOKE: PASS (unit only; set CBOOT_TEST_MYSQL_DATABASE for integration)");
        return 0;
    }

    try {
        orm::MysqlProps cfg;
        if (const char* host = std::getenv("CBOOT_TEST_MYSQL_HOST"))
            cfg.host = host;
        if (const char* port = std::getenv("CBOOT_TEST_MYSQL_PORT"))
            cfg.port = std::atoi(port);
        if (const char* user = std::getenv("CBOOT_TEST_MYSQL_USER"))
            cfg.user = user;
        if (const char* password = std::getenv("CBOOT_TEST_MYSQL_PASSWORD"))
            cfg.password = password;
        cfg.database = database;
        cfg.pool_size = 1;
        cfg.acquire_timeout_ms = 3000;

        health_registry health;
        orm::MysqlDataSource ds(cfg, health);
        ds.open();

        orm::EntityManager db(ds);
        db.drop_schema<MysqlUser>();
        db.create_schema<MysqlUser>();

        MysqlUser u{.name = "alice", .active = true};
        db.insert(u);
        if (u.id <= 0) return 16;

        auto found = db.find<MysqlUser>(u.id);
        if (!found || found->name != "alice" || found->email.has_value())
            return 17;

        auto selected = db.select<MysqlUser>()
            .where(orm::col<^^MysqlUser::active>() == true)
            .order_by(orm::desc(orm::col<^^MysqlUser::id>()))
            .fetch();
        if (selected.size() != 1 || selected.front().id != u.id) return 18;

        found->name = "bob";
        found->email = std::string("bob@example.com");
        if (db.update(*found) != 1) return 19;

        auto updated = db.find<MysqlUser>(u.id);
        if (!updated || updated->name != "bob"
            || updated->email != "bob@example.com") return 20;

        if (db.remove<MysqlUser>(u.id) != 1) return 21;
        if (db.find<MysqlUser>(u.id).has_value()) return 22;

        db.drop_schema<MysqlUser>();
        ds.close();
    } catch (const std::exception& e) {
        std::println("ORM MySQL integration failed: {}", e.what());
        return 23;
    }

    std::println("ORM MYSQL SMOKE: PASS");
    return 0;
}
