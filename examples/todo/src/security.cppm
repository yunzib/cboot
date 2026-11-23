/**
 * @file security.cppm
 * @brief 密码哈希 — PBKDF2-HMAC-SHA256
 *
 * 编码格式：pbkdf2-sha256$<iterations>$<hex salt>$<hex hash>
 * 仅示例使用；生产应使用 Argon2/bcrypt 等专用密码哈希库。
 */
export module todo.security;

import std;

export namespace todo::security {

/** @brief 生成带随机盐的密码哈希 */
std::string hash_password(std::string_view password);

/** @brief 校验密码；编码串非法或密码错误都返回 false */
bool verify_password(std::string_view password, std::string_view encoded);

} // namespace todo::security
