/**
 * @file auth.postgres.cppm
 * @brief PostgreSQL TokenStore 驱动
 *
 * 多实例/重启保持登录态时，用 PostgreSQL 表替代进程内 MemoryTokenStore。
 * 依赖 auth_component（AuthService/AuthGuard）与 postgres_component
 * （DataSource/EntityManager）；应用选择 auth_postgres_component 即可。
 */
export module cboot.auth.postgres;

import std;
import cboot.auth;              // TokenStore / session / new_token / auth_component
import cboot.core.annotations;  // Bean / PostConstruct / bean_name
import cboot.meta.reflect;      // type_list
import cboot.orm;               // EntityManager / Table / Id
import cboot.orm.postgres;      // postgres_component
import cboot.json;

export namespace auth {

/**
 * @brief PostgreSQL 持久化 TokenStore
 *
 * 表 cboot_auth_sessions：
 *   token TEXT PRIMARY KEY, subject TEXT, roles_json TEXT, scopes_json TEXT,
 *   issued_at BIGINT, expires_at BIGINT, device TEXT, kind TEXT
 *
 * 启动时 [[=PostConstruct]] 建表、subject 索引，并给旧表自动补
 * issued_at / kind 列（ALTER TABLE IF NOT EXISTS）；旧行 kind 空串
 * 视同 access。
 */
class [[=Bean{}]] PgTokenStore final : public TokenStore {
public:
    explicit PgTokenStore(orm::EntityManager& db);
    ~PgTokenStore() override;

    std::string create(session s) override;
    std::optional<session> get(std::string_view token) const override;
    void revoke(std::string_view token) override;
    void revoke_subject(std::string_view subject) override;
    bool touch(std::string_view token, std::int64_t expires_at) override;

    /** @brief 建表/索引；PostConstruct 自动调用 */
    [[=PostConstruct{}]] void ensure_schema();

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace auth

/**
 * @brief PostgreSQL 登录态组件
 *
 * 依赖 auth_component + postgres_component；
 * 与 auth_memory_component 二选一。
 */
export struct auth_postgres_component {
    static constexpr bean_name name{"auth.postgres"};
    using beans = type_list<auth::PgTokenStore>;
    using dependencies = type_list<auth_component, postgres_component>;
};
