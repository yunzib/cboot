export module cboot.web.http;

import std;
export import cboot.json;

/**
 * @brief HTTP 词汇 — 方法词汇 + 路由注解 + Request/Response
 *
 * 路由注解（行为键，挂在方法上）：Get / Post / Put / Delete / Patch。
 * 所在类必须是显式 [[=Bean{}]]（单一规则，见 cboot.app 扫描校验）。
 *
 * 路径参数："/users/{id}/{field}" —— '{' 起始段为参数段；
 * handler 参数按序消费（数值/字符串为路径参数，聚合体 = JSON body，
 * Request = 请求本体）。路径段与 query 值均做百分号解码。
 */

// ==================== 方法 ====================

export enum class http_method : std::uint8_t { get, post, put, del, patch };

/** @brief 方法名（"GET"/"POST"/"PUT"/"DELETE"/"PATCH"；未知返回空） */
export constexpr std::string_view method_name(http_method m) {
    switch (m) {
    case http_method::get:   return "GET";
    case http_method::post:  return "POST";
    case http_method::put:   return "PUT";
    case http_method::del:   return "DELETE";
    case http_method::patch: return "PATCH";
    }
    return {};
}

/** @brief 方法名 → 方法（大小写敏感；未知 nullopt） */
export constexpr std::optional<http_method> method_from_name(std::string_view s) {
    for (http_method m : {http_method::get, http_method::post, http_method::put,
                          http_method::del, http_method::patch})
        if (method_name(m) == s) return m;
    return std::nullopt;
}

// ==================== 路由注解 ====================

/** @brief 路径注解值：structural 聚合（char[128]；超长编译错） */
export struct path_name {
    char data[128]{};
    constexpr std::string_view view() const { return std::string_view(data); }
};

/** @brief GET 路由注解：[[=Get{"/users/{id}"}]] */
export struct Get { path_name path{}; };
/** @brief POST 路由注解：[[=Post{"/users"}]] */
export struct Post { path_name path{}; };
/** @brief PUT 路由注解：[[=Put{"/users/{id}"}]] */
export struct Put { path_name path{}; };
/** @brief DELETE 路由注解：[[=Delete{"/users/{id}"}]] */
export struct Delete { path_name path{}; };
/** @brief PATCH 路由注解：[[=Patch{"/users/{id}"}]] */
export struct Patch { path_name path{}; };

// ==================== URL 解码 ====================

/**
 * @brief 百分号解码（%XX）；plus_as_space=true 时 '+' → ' '（表单 query 口径）
 * 非法转义（% 后非两位十六进制）原样保留，不抛异常
 */
export inline std::string url_decode(std::string_view s, bool plus_as_space = false) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()
            && std::isxdigit(static_cast<unsigned char>(s[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            const auto hex = [](char h) {
                return (h >= '0' && h <= '9') ? h - '0'
                     : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                              : h - 'A' + 10;
            };
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else if (c == '+' && plus_as_space) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/** @brief 百分号解码并追加到 out（零临时分配；语义同 url_decode） */
export inline void url_decode_append(std::string& out, std::string_view s,
                                     bool plus_as_space = false) {
    const auto hex = [](char h) {
        return (h >= '0' && h <= '9') ? h - '0'
             : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                      : h - 'A' + 10;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()
            && std::isxdigit(static_cast<unsigned char>(s[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else if (c == '+' && plus_as_space) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
}

// ==================== Request / Response ====================

/**
 * @brief 请求视图（零拷贝）：字段指向解析器缓冲 / 路由解码 scratch，
 *        有效期仅限本次 handler 调用（不要在 handler 外保存）。
 */
export struct Request {
    std::string_view method;   // "GET" / "POST" / ...
    std::string_view path;     // 解码后的路径（不含 query）
    std::string_view query;    // 原始 query（值未解码）
    std::string_view body;     // 原始 body（JSON 等）

    /** @brief query 参数取值（原始未解码视图；未命中 nullopt） */
    std::optional<std::string_view> query_raw(std::string_view name) const {
        std::size_t i = 0;
        while (i < query.size()) {
            std::size_t amp = query.find('&', i);
            if (amp == std::string::npos) amp = query.size();
            std::string_view kv(query.data() + i, amp - i);
            std::size_t eq = kv.find('=');
            std::string_view k = kv.substr(0, eq);
            if (k == name)
                return eq == std::string_view::npos ? std::string_view{}
                                                    : kv.substr(eq + 1);
            i = amp + 1;
        }
        return std::nullopt;
    }

    /** @brief query 参数取值（百分号/+ 解码；未命中 nullopt） */
    std::optional<std::string> query_get(std::string_view name) const {
        if (auto raw = query_raw(name)) return url_decode(*raw, /*plus_as_space=*/true);
        return std::nullopt;
    }
};

export struct Response {
    int status = 200;
    // 视图（静态字面量存储）：避免每次响应构造的一次堆分配（热路径）
    std::string_view content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;   // 自定义响应头
    // 内联 64 字节：小响应体（多数 JSON）零堆分配；仅扩大 64 字节栈占用
    json::small_buffer<64> body;

    static Response text(std::string s) {
        Response r;
        r.content_type = "text/plain; charset=utf-8";
        r.body = std::move(s);
        return r;
    }

    /** @brief 链式追加响应头 */
    Response& header(std::string name, std::string value) {
        headers.emplace_back(std::move(name), std::move(value));
        return *this;
    }
};

/** @brief JSON 响应便捷构造（value 形态） */
export Response json_response(const json::value& v) {
    Response r;
    r.body = json::dump(v);
    return r;
}

/** @brief JSON 响应便捷构造（反射序列化形态） */
export template <class T>
Response json_response(const T& t) {
    Response r;
    json::write_to(r.body, t);      // 流式序列化：不建 value 树（热路径）
    return r;
}
