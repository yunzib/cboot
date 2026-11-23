/**
 * @file service.cppm
 * @brief 业务层 — 待办与账号，均不依赖 HTTP
 */
export module todo.service;

import std;
import cboot.auth;             // AuthService / session
import cboot.core.annotations; // Bean
import cboot.web.middleware;   // request_context（logout）
import todo.model;
import todo.repository;
import todo.security;

/** @brief 待办业务服务；所有操作按 owner 隔离 */
export class [[=Bean{}]] TaskService {
public:
    explicit TaskService(TaskRepository& repo) : repo_(repo) {}

    std::vector<todo::Task> list(std::string_view owner) {
        return repo_.all(owner);
    }

    todo::Task add(std::string_view owner, std::string title) {
        return repo_.create(std::string(owner), std::move(title));
    }

    std::optional<todo::Task> set_done(std::string_view owner, std::int64_t id,
                                       bool done) {
        auto task = repo_.find(id);
        if (!task || task->owner != owner) return std::nullopt;

        task->done = done;
        if (!repo_.update(*task)) return std::nullopt;
        return task;
    }

    bool remove(std::string_view owner, std::int64_t id) {
        auto task = repo_.find(id);
        if (!task || task->owner != owner) return false;
        return repo_.remove(id);
    }

private:
    TaskRepository& repo_;
};

/** @brief 账号业务：校验密码并创建登录态 */
export class [[=Bean{}]] AccountService {
public:
    AccountService(UserRepository& users, auth::AuthService& sessions)
        : users_(users), sessions_(sessions) {}

    /** @brief 成功返回 token；用户名或密码错误返回 nullopt */
    std::optional<std::string> login(std::string_view username,
                                     std::string_view password) {
        auto user = users_.find_by_username(username);
        if (!user || !todo::security::verify_password(password, user->password_hash))
            return std::nullopt;

        return sessions_.login(auth::session{.subject = user->username});
    }

    void logout(const web::request_context& ctx) { sessions_.logout(ctx); }

private:
    UserRepository& users_;
    auth::AuthService& sessions_;
};
