/**
 * @file main.cpp
 * @brief ORM 契约组件示例
 *
 * 演示：
 *   - 实体注解 Table / Column / Id
 *   - 应用选择 postgres_component（依赖闭包带入 orm_component）
 *   - EntityManager CRUD 与类型安全 DSL
 *   - 启动时自动建表（仅示例；生产用迁移）
 *
 * 运行前需要 PostgreSQL，并设置 PGPASSWORD 或 app.json 的 db.password。
 */
import cboot.app;
import cboot.orm.postgres;
import std;

/**
 * @brief 示例实体
 *
 * 表名显式写 schema=public，避免依赖 search_path。
 * id 自增，INSERT 时由 RETURNING 回填。
 */
struct [[=orm::Table{.name = "cboot_example_users", .schema = "public"}]] User {
    [[=orm::Id{.generated = true}]] std::int64_t id{};
    [[=orm::Column{"display_name"}]] std::string name;
    std::optional<std::string> email;
    bool active{};
};

/** @brief POST body；和实体分开，避免客户端传 id */
struct CreateUser {
    std::string name;
    std::optional<std::string> email;
    bool active{true};
};

/**
 * @brief 启动建表
 *
 * 示例工程自动 create_schema；真实项目应使用迁移脚本。
 * OrmSetup 依赖 EntityManager，拓扑序保证 DataSource 已 open。
 */
class [[=Bean{}]] OrmSetup {
public:
    OrmSetup(orm::EntityManager& db) : db_(db) {}

    [[=PostConstruct{}]] void create_schema() {
        db_.create_schema<User>();
    }

private:
    orm::EntityManager& db_;
};

/**
 * @brief 用户 CRUD 控制器
 *
 * 控制器也是 bean；构造函数注入 EntityManager。
 */
class [[=Bean{}]] UserApi {
public:
    UserApi(orm::EntityManager& db) : db_(db) {}

    /** @brief GET /api/orm/users/{id} */
    [[=Get{"/api/orm/users/{id}"}]]
    Response get(std::int64_t id) {
        auto user = db_.find<User>(id);
        if (!user) return not_found();
        return json_response(*user);
    }

    /** @brief GET /api/orm/users：类型安全 DSL 列表查询 */
    [[=Get{"/api/orm/users"}]]
    Response list() {
        auto rows = db_.select<User>()
            .order_by(orm::asc(orm::col<^^User::id>()))
            .fetch();
        return json_response(rows);
    }

    /** @brief POST /api/orm/users */
    [[=Post{"/api/orm/users"}]]
    Response create(const CreateUser& in) {
        User user{.name = in.name, .email = in.email, .active = in.active};
        db_.insert(user);                      // 自增 id 回填到 user.id
        Response r = json_response(user);
        r.status = 201;
        return r;
    }

    /** @brief DELETE /api/orm/users/{id} */
    [[=Delete{"/api/orm/users/{id}"}]]
    Response remove(std::int64_t id) {
        if (db_.remove<User>(id) == 0) return not_found();
        Response r;
        r.body = R"({"deleted":true})";
        return r;
    }

private:
    static Response not_found() {
        Response r = Response::text("not found");
        r.status = 404;
        return r;
    }

    orm::EntityManager& db_;
};

/** @brief 应用：选择 PostgreSQL 驱动组件 */
class OrmApp : public application<OrmApp> {
public:
    static constexpr std::string_view config_path = "app.json";
    static constexpr unsigned short default_port = 8080;
    using components = type_list<postgres_component>;
};

int main(int argc, char** argv) {
    // 不传端口时走 app.json / default_port；传 0 交给配置
    const unsigned short port = argc > 1
        ? static_cast<unsigned short>(std::atoi(argv[1])) : 0;
    return OrmApp{}.port(port).run();
}
