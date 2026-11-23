export module cboot.web.session;

import std;
import cboot.web.http;
import cboot.web.parser;
import cboot.web.routing;
import cboot.web.error;

/**
 * @brief HTTP 会话层 — 传输无关的"字节进 → 字节出"协议核心
 *
 * 线程后端（阻塞 recv）与 IOCP 后端（完成通知）共用：
 *   后端把收到的字节喂给 on_bytes()，取走待发送字节与"是否关闭"。
 * 负责：增量解析（pipelining）/ 路由派发 / 响应序列化 / keep-alive 决策 /
 *       单连接请求数上限。
 */

namespace {

/**
 * @brief connector 层错误（解析 400/413、handler 超时 504）
 *
 * 这些错误发生在进入 middleware 之前或 SLA 判定处，和 Servlet 容器
 * 的解析错误一样不经过 ErrorHandler；使用默认错误响应。
 */
Response connector_error(int status, std::string code, std::string message) {
    web::error_info info{
        .status = status,
        .code = std::move(code),
        .message = std::move(message),
    };
    return web::default_error_response(info);
}

} // namespace

export std::string_view http_status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Status";
    }
}

export Response http_make_error(int status, std::string_view text) {
    Response r = Response::text(std::string(text));
    r.status = status;
    return r;
}

/** @brief 序列化响应（追加到 out；keep_alive 决定连接头） */
export void http_append_response(std::string& out, const Response& resp, bool keep_alive) {
    out.append("HTTP/1.1 ");
    out.append(std::to_string(resp.status));
    out.push_back(' ');
    out.append(http_status_text(resp.status));
    out.append("\r\nContent-Type: ");
    out.append(resp.content_type);
    out.append("\r\nContent-Length: ");
    out.append(std::to_string(resp.body.size()));
    out.append("\r\n");
    for (const auto& [name, value] : resp.headers) {
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }
    if (keep_alive) out.append("Connection: keep-alive\r\nKeep-Alive: timeout=15\r\n");
    else out.append("Connection: close\r\n");
    out.append("\r\n");
    out.append(resp.body.data(), resp.body.size());
}

export class http_session {
public:
    static constexpr std::size_t max_requests_per_connection = 1024;

    /**
     * @param handler_timeout_ms 单请求 handler 预算（0 = 不限）。超时返回 504：
     *   同步 handler 无法安全抢占，本项为 **SLA 语义**（超时即判定失败），
     *   而非中断执行；真正的执行级超时需异步 handler + 取消（后续项）。
     */
    explicit http_session(const router& r, std::size_t handler_timeout_ms = 0)
        : router_(r), handler_timeout_ms_(handler_timeout_ms) {}

    /** @brief 喂入字节；out 追加响应；返回 true = 应关闭连接 */
    bool on_bytes(std::string_view data, std::string& out, bool stopping) {
        parser_.feed(data);
        parsed_request rq;
        while (true) {
            switch (parser_.next(rq)) {
            case request_parser::result::need_more:
                return false;
            case request_parser::result::error:
                http_append_response(out,
                    connector_error(400, "bad_request", "bad request"), false);
                return true;
            case request_parser::result::too_large:
                http_append_response(out,
                    connector_error(413, "payload_too_large",
                                    "payload too large"), false);
                return true;
            case request_parser::result::complete:
                break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Response resp;
            try {
                resp = router_.dispatch(rq.method, rq.target, rq.body, rq.headers);
            } catch (const std::exception& e) {
                resp = connector_error(500, "internal", e.what());
            } catch (...) {
                resp = connector_error(500, "internal", "internal error");
            }
            parser_.consume(rq.consumed);    // 视图有效期至此结束
            if (handler_timeout_ms_ != 0) {
                const auto us = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (static_cast<std::size_t>(us) > handler_timeout_ms_)
                    resp = connector_error(504, "gateway_timeout",
                                           "gateway timeout (handler budget)");
            }
            const bool keep = rq.keep_alive && !stopping
                           && ++served_ < max_requests_per_connection;
            http_append_response(out, resp, keep);
            if (!keep) return true;
        }
    }

    /** @brief 是否有未消费的残留字节（半包） */
    bool pending() const { return parser_.pending_bytes() > 0; }

    /** @brief 已处理请求数 */
    std::size_t served() const { return served_; }

private:
    request_parser parser_;
    const router& router_;
    std::size_t handler_timeout_ms_ = 0;
    std::size_t served_ = 0;
};
