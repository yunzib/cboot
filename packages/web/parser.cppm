export module cboot.web.parser;

import std;

/**
 * @brief 增量 HTTP/1.1 请求解析器（协议核心，传输层无关）
 *
 * 零拷贝语义（性能关键）：
 *   - parsed_request 的 method/target/body 均为指向内部缓冲的 **string_view**
 *   - next() 只定位、不消费；调用方处理完（路由派发/响应）后调用 consume(n)
 *     才从缓冲头部移除——试图在这些视图仍被使用时保持数据有效
 *   - 单遍大小写不敏感头部扫描（content-length / connection），零分配
 *
 * 语义：
 *   - 支持 pipelining：next() 循环取尽缓冲中的完整请求；残留数据保留
 *   - 定界：头部以 CRLFCRLF 结束；body 以 Content-Length 界定
 *   - 限额：头部 64 KB / body 1 MB（超出返回 too_large，由调用方回 413）
 *   - 连接复用：HTTP/1.1 默认 keep-alive；`Connection: close` 关闭
 */

/** @brief 解析产出的请求视图（视图有效期：到 consume() 之前） */
export struct parsed_request {
    std::string_view method;      // 视图 → 解析器缓冲
    std::string_view target;      // 原始 target（路径 + query）
    std::string_view headers;     // 原始 header 块（请求行之后、空行之前）
    std::string_view body;        // 原始 body
    std::size_t consumed = 0;     // 本请求占用字节数（处理完传给 consume）
    bool keep_alive = true;       // 连接复用意愿（版本 + Connection 头）
};

export class request_parser {
public:
    static constexpr std::size_t max_header_bytes = 64 * 1024;
    static constexpr std::size_t max_body_bytes = 1024 * 1024;

    enum class result { complete, need_more, error, too_large };

    /** @brief 追加到达的字节（可任意切分） */
    void feed(std::string_view data) { buf_.append(data); }

    /** @brief 尝试定位下一个完整请求（不消费缓冲） */
    result next(parsed_request& out) {
        const std::size_t he = buf_.find("\r\n\r\n", pos_);
        if (he == std::string::npos) {
            if (buf_.size() - pos_ > max_header_bytes) return result::too_large;
            return result::need_more;
        }
        const std::string_view head(buf_.data() + pos_, he - pos_);
        const std::size_t sp1 = head.find(' ');
        const std::size_t sp2 = sp1 == std::string_view::npos
                              ? std::string_view::npos : head.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos) return result::error;

        // 单遍头扫描（content-length + connection 一次收集）
        const header_info hi = scan_headers(head);
        const std::size_t cl = hi.content_length;
        if (cl > max_body_bytes) return result::too_large;
        if (buf_.size() - pos_ < he - pos_ + 4 + cl) return result::need_more;

        out.method = head.substr(0, sp1);
        out.target = head.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::size_t first_crlf = head.find("\r\n");
        out.headers = first_crlf == std::string_view::npos
            ? std::string_view{}
            : head.substr(first_crlf + 2);
        out.body = std::string_view(buf_.data() + he + 4, cl);
        out.consumed = he + 4 + cl - pos_;
        // HTTP/1.1 默认 keep-alive；Connection 头显式给出时以其为准
        const bool http11 = head.substr(sp2 + 1, 8) == "HTTP/1.1";
        out.keep_alive = hi.has_connection ? !hi.connection_close : http11;
        return result::complete;
    }

    /**
     * @brief 处理完成后消费已解析的字节
     *
     * 性能关键（pipelining）：**不做每请求 erase**（否则剩余缓冲反复搬移，
     * 一次 8KB 读含数十个请求时是 O(n²)）。改为消费偏移；仅当缓冲排空或
     * 已消费过半且超过阈值时才压缩一次（均摊 O(1)）。
     * 压缩发生在 dispatch 返回之后——此时请求视图已不再被使用。
     */
    void consume(std::size_t n) {
        pos_ += n;
        if (pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
        } else if (pos_ > 4096 && pos_ > buf_.size() / 2) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
    }

    /** @brief 尚未消费的字节数（用于超时/背压判断） */
    std::size_t pending_bytes() const { return buf_.size() - pos_; }

private:
    std::string buf_;
    std::size_t pos_ = 0;      // 已消费前缀偏移（不搬移，accepted）

    // ── 头部扫描：单遍、大小写不敏感、零分配 ──
    // 性能关键：算术小写化（无 libc 调用）+ 一次遍历收集全部需要的头，
    // 而不是每个头单独 ifind（否则每请求数十次 std::tolower 调用）。

    static char lower_ascii(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    /** @brief line 是否以 name + ':' 开头（大小写不敏感） */
    static bool header_name_is(std::string_view line, std::string_view name) {
        if (line.size() <= name.size()) return false;
        if (line[name.size()] != ':') return false;
        for (std::size_t i = 0; i < name.size(); ++i)
            if (lower_ascii(line[i]) != name[i]) return false;
        return true;
    }

    struct header_info {
        std::size_t content_length = 0;
        bool has_connection = false;
        bool connection_close = false;
    };

    /**
     * @brief 单遍扫描头部各行，收集 content-length / connection
     * （跳过请求行；无分配；行数通常 1–4）
     */
    static header_info scan_headers(std::string_view head) {
        header_info hi;
        std::size_t line = head.find("\r\n");
        while (line != std::string_view::npos && line + 2 < head.size()) {
            const std::size_t next = head.find("\r\n", line + 2);
            const std::size_t end = next == std::string_view::npos ? head.size() : next;
            const std::string_view hline = head.substr(line + 2, end - line - 2);

            if (header_name_is(hline, "content-length")) {
                std::size_t v = 15;    // 跳过 "content-length:"
                while (v < hline.size() && (hline[v] == ' ' || hline[v] == '\t')) ++v;
                std::from_chars(hline.data() + v, hline.data() + hline.size(),
                                hi.content_length);
            } else if (header_name_is(hline, "connection")) {
                hi.has_connection = true;
                std::size_t v = 11;    // 跳过 "connection:"
                while (v < hline.size() && (hline[v] == ' ' || hline[v] == '\t')) ++v;
                const std::string_view val = hline.substr(v);
                for (std::size_t i = 0; i + 5 <= val.size(); ++i) {
                    bool m = true;
                    for (std::size_t k = 0; k < 5; ++k)
                        if (lower_ascii(val[i + k]) != "close"[k]) { m = false; break; }
                    if (m) hi.connection_close = true;
                }
            }
            line = next;
        }
        return hi;
    }
};
