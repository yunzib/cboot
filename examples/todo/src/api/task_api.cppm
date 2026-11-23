/**
 * @file task_api.cppm
 * @brief REST 层 — 只做请求/响应映射，不写业务逻辑
 */
export module todo.api.task;

import std;
import cboot.auth;              // session
import cboot.core.annotations;  // Bean
import cboot.web.http;          // Get / Post / Patch / Delete / Response
import cboot.web.middleware;    // request_context
import todo.model;
import todo.service;

/**
 * @brief 待办 REST 接口
 *
 * 所有路由受 AuthGuard 保护：认证失败由 guard 统一抛出结构化错误，
 * 不会进入 handler。因此这里直接取 session，不重复判断“未登录”。
 */
export class [[=Bean{}]] TaskApi {
public:
    explicit TaskApi(TaskService& service) : service_(service) {}

    /** @brief GET /api/tasks */
    [[=Get{"/api/tasks"}]]
    Response list(const web::request_context& ctx) {
        const auto& session = ctx.get<auth::session>();
        return json_response(service_.list(session.subject));
    }

    /** @brief POST /api/tasks */
    [[=Post{"/api/tasks"}]]
    Response create(const web::request_context& ctx, const todo::CreateTask& in) {
        const auto& session = ctx.get<auth::session>();
        Response r = json_response(service_.add(session.subject, in.title));
        r.status = 201;
        return r;
    }

    /** @brief PATCH /api/tasks/{id} */
    [[=Patch{"/api/tasks/{id}"}]]
    Response update(const web::request_context& ctx, std::int64_t id,
                    const todo::UpdateTask& in) {
        const auto& session = ctx.get<auth::session>();
        auto task = service_.set_done(session.subject, id, in.done);
        if (!task) return not_found();
        return json_response(*task);
    }

    /** @brief DELETE /api/tasks/{id} */
    [[=Delete{"/api/tasks/{id}"}]]
    Response remove(const web::request_context& ctx, std::int64_t id) {
        const auto& session = ctx.get<auth::session>();
        if (!service_.remove(session.subject, id)) return not_found();
        Response r;
        r.body = R"({"deleted":true})";
        return r;
    }

private:
    static Response not_found() {
        Response r = Response::text("task not found");
        r.status = 404;
        return r;
    }

    TaskService& service_;
};
