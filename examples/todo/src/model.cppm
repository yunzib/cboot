/**
 * @file model.cppm
 * @brief 领域模型与 DTO
 */
export module todo.model;

import std;
import cboot.orm.postgres;      // Table / Id / Column
import cboot.validation;        // NotBlank

export namespace todo {

/**
 * @brief 待办实体
 *
 * id 自增，INSERT 时由 RETURNING 回填；done 映射 PostgreSQL BOOLEAN。
 * owner 用列名 owner_name，避免与 PostgreSQL 关键字混淆。
 */
struct [[=orm::Table{.name = "cboot_demo_tasks", .schema = "public"}]] Task {
    [[=orm::Id{.generated = true}]] std::int64_t id{};
    [[=orm::Column{"owner_name"}]] std::string owner;
    std::string title;
    bool done{};
};

/** @brief 登录用户；password_hash 为 PBKDF2 编码串，不存明文 */
struct [[=orm::Table{.name = "cboot_demo_users", .schema = "public"}]] User {
    [[=orm::Id{.generated = true}]] std::int64_t id{};
    std::string username;
    std::string password_hash;
};

// ==================== 请求 / 响应 DTO ====================

/** @brief POST /api/tasks；title 空白由 validation 自动返回 400 */
struct CreateTask {
    [[=validation::NotBlank{"title 不能为空"}]] std::string title;
};

/** @brief PATCH /api/tasks/{id} */
struct UpdateTask {
    bool done{};
};

/** @brief POST /api/auth/login */
struct LoginRequest {
    [[=validation::NotBlank{"username 不能为空"}]] std::string username;
    [[=validation::NotBlank{"password 不能为空"}]] std::string password;
};

/** @brief 登录成功响应 */
struct LoginResponse {
    std::string token;
    std::string username;
};

/** @brief GET /api/auth/me 响应 */
struct MeResponse {
    std::string username;
};

} // namespace todo
