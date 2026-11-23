/**
 * @file auth_api.cppm
 * @brief 登录 / 登出 / 当前用户
 */
export module todo.api.auth;

import std;
import cboot.auth;              // AuthService / session
import cboot.core.annotations;  // Bean
import cboot.web.error;         // web::error
import cboot.web.http;          // Post / Get / Response
import cboot.web.middleware;    // request_context
import todo.model;
import todo.service;

/** @brief 认证接口；登录路由由 auth.public_paths 放行，其余受保护 */
export class [[=Bean{}]] AuthApi {
public:
    AuthApi(AccountService& accounts, auth::AuthService& sessions)
        : accounts_(accounts), sessions_(sessions) {}

    /** @brief POST /api/auth/login */
    [[=Post{"/api/auth/login"}]]
    Response login(const todo::LoginRequest& in) {
        auto token = accounts_.login(in.username, in.password);
        if (!token) {
            web::error_info info;
            info.status = 401;
            info.code = "auth.invalid_credentials";
            info.message = "invalid credentials";
            info.method = "POST";
            info.path = "/api/auth/login";
            info.headers.emplace_back("WWW-Authenticate", "Bearer");
            throw web::error(std::move(info));
        }

        Response r = json_response(todo::LoginResponse{
            .token = *token,
            .username = in.username,
        });
        sessions_.attach_cookie(r, *token);   // 配置 token_cookie 时同时下发
        return r;
    }

    /** @brief POST /api/auth/logout */
    [[=Post{"/api/auth/logout"}]]
    Response logout(const web::request_context& ctx) {
        accounts_.logout(ctx);

        Response r;
        sessions_.clear_cookie(r);
        return r;
    }

    /** @brief GET /api/auth/me */
    [[=Get{"/api/auth/me"}]]
    Response me(const web::request_context& ctx) {
        const auto& session = ctx.get<auth::session>();
        return json_response(todo::MeResponse{.username = session.subject});
    }

private:
    AccountService& accounts_;
    auth::AuthService& sessions_;
};
