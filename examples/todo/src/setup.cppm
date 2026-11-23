/**
 * @file setup.cppm
 * @brief 示例启动建表 + 默认账号
 */
export module todo.setup;

import std;
import cboot.core.annotations;  // Bean / PostConstruct
import cboot.orm.postgres;      // EntityManager
import todo.model;
import todo.repository;
import todo.security;

/**
 * @brief 启动建表与种子数据
 *
 * 仅示例使用；生产应改用迁移脚本，并替换默认账号。
 * PostConstruct 在 DataSource 打开后执行。
 */
export class [[=Bean{}]] Setup {
public:
    Setup(orm::EntityManager& db, UserRepository& users)
        : db_(db), users_(users) {}

    [[=PostConstruct{}]] void initialize() {
        db_.create_schema<todo::Task>();
        db_.create_schema<todo::User>();
        db_.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS cboot_demo_users_username_idx"
            " ON public.cboot_demo_users (username)",
            {});

        // 演示账号：admin / admin123。生产必须删除或改密。
        if (!users_.find_by_username("admin")) {
            users_.create(todo::User{
                .username = "admin",
                .password_hash = todo::security::hash_password("admin123"),
            });
        }
    }

private:
    orm::EntityManager& db_;
    UserRepository& users_;
};
