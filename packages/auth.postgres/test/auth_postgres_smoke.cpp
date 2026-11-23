/**
 * @file auth_postgres_smoke.cpp
 * @brief PostgreSQL TokenStore 集成冒烟测试
 *
 * 需要 CBOOT_TEST_PG_URL；未设置时只打印跳过信息。
 */
import cboot.auth;
import cboot.auth.postgres;
import cboot.core.context;
import cboot.orm;
import cboot.orm.postgres;
import std;

int main() {
    const char* url = std::getenv("CBOOT_TEST_PG_URL");
    if (!url || !*url) {
        std::println("AUTH POSTGRES SMOKE: PASS (unit only; set CBOOT_TEST_PG_URL)");
        return 0;
    }

    try {
        orm::PgProps cfg;
        cfg.url = url;
        cfg.pool_size = 1;

        health_registry health;
        orm::PgDataSource ds(cfg, health);
        ds.open();

        orm::EntityManager db(ds);
        db.execute("DROP TABLE IF EXISTS "
                   + db.dialect().quote_identifier("cboot_auth_sessions"), {});

        auth::PgTokenStore store(db);
        store.ensure_schema();

        const std::string token = store.create(auth::session{
            .subject = "user-42",
            .roles = {"admin"},
            .scopes = {"read"},
        });
        auto loaded = store.get(token);
        if (!loaded || loaded->subject != "user-42"
            || loaded->roles.size() != 1 || loaded->roles[0] != "admin"
            || loaded->scopes.size() != 1 || loaded->scopes[0] != "read")
            return 1;

        store.revoke(token);
        if (store.get(token).has_value()) return 2;

        const std::string t1 = store.create(auth::session{.subject = "kick"});
        const std::string t2 = store.create(auth::session{.subject = "kick"});
        store.revoke_subject("kick");
        if (store.get(t1).has_value() || store.get(t2).has_value()) return 3;

        std::println("AUTH POSTGRES SMOKE: PASS");
        ds.close();
    } catch (const std::exception& e) {
        std::println("AUTH POSTGRES integration failed: {}", e.what());
        return 4;
    }
    return 0;
}
