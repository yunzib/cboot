/**
 * @file errors.cppm
 * @brief 自定义错误响应 — 给 401/403 增加机器可读 code
 *
 * 只处理认证/授权错误，其余返回 nullopt 回退默认响应。
 */
export module todo.api.errors;

import std;
import cboot.core.annotations;  // Bean
import cboot.web.error;         // ErrorMapper / error_info
import cboot.web.http;          // Response / json_response / json
import cboot.web.middleware;    // request_context

/** @brief 全局错误 mapper；order 小的先执行 */
export class [[=Bean{}]] [[=web::ErrorMapper{.order = 10}]] ApiErrors {
public:
    std::optional<Response> handle(const web::error_info& info,
                                   const web::request_context&) {
        if (info.status != 401 && info.status != 403) return std::nullopt;

        json::object body;
        body.emplace_back("error", json::value(
            info.status == 401 ? "unauthorized" : "forbidden"));
        body.emplace_back("code", json::value(info.code));

        Response r = json_response(json::value(std::move(body)));
        r.status = info.status;
        return r;
    }
};
