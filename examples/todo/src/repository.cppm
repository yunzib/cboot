/**
 * @file repository.cppm
 * @brief 数据访问层 — 只负责 Task/User 的持久化
 *
 * Bean 位于全局命名空间：应用 TU 通过自动扫描发现（与普通应用一致）。
 */
export module todo.repository;

import std;
import cboot.core.annotations;  // Bean
import cboot.orm.postgres;      // EntityManager / DSL
import todo.model;

/** @brief 待办仓储；业务逻辑不直接接触 EntityManager */
export class [[=Bean{}]] TaskRepository {
public:
    explicit TaskRepository(orm::EntityManager& db) : db_(db) {}

    std::vector<todo::Task> all(std::string_view owner) {
        return db_.select<todo::Task>()
            .where(orm::col<^^todo::Task::owner>() == std::string(owner))
            .order_by(orm::asc(orm::col<^^todo::Task::id>()))
            .fetch();
    }

    std::optional<todo::Task> find(std::int64_t id) {
        return db_.find<todo::Task>(id);
    }

    todo::Task create(std::string owner, std::string title) {
        todo::Task task{.owner = std::move(owner), .title = std::move(title)};
        db_.insert(task);       // 自增 id 回填
        return task;
    }

    bool update(const todo::Task& task) { return db_.update(task) > 0; }

    bool remove(std::int64_t id) { return db_.remove<todo::Task>(id) > 0; }

private:
    orm::EntityManager& db_;
};

/** @brief 登录用户仓储 */
export class [[=Bean{}]] UserRepository {
public:
    explicit UserRepository(orm::EntityManager& db) : db_(db) {}

    std::optional<todo::User> find_by_username(std::string_view username) {
        auto rows = db_.select<todo::User>()
            .where(orm::col<^^todo::User::username>() == std::string(username))
            .limit(1)
            .fetch();
        if (rows.empty()) return std::nullopt;
        return std::move(rows.front());
    }

    void create(todo::User user) { db_.insert(user); }

private:
    orm::EntityManager& db_;
};
