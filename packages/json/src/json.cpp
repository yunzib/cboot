// cboot.json 实现单元 — 解析器 / 序列化器（接口声明见 modules/json/json.cppm）
module cboot.json;

import std;

namespace json {
namespace detail {

struct parser {
    std::string_view src;
    std::size_t pos = 0;
    std::size_t max_depth = 128;
    std::size_t depth = 0;

    [[noreturn]] void fail(std::string_view what) {
        // 行列按当前 pos 计算；源文本只做一次线性扫描（错误路径）
        std::size_t line = 1;
        std::size_t column = 1;
        const std::size_t end = pos < src.size() ? pos : src.size();
        for (std::size_t i = 0; i < end; ++i) {
            if (src[i] == '\n') { ++line; column = 1; }
            else ++column;
        }
        throw error(std::string("cboot.json: ") + std::string(what)
                    + " at line " + std::to_string(line)
                    + ", column " + std::to_string(column)
                    + " (offset " + std::to_string(pos) + ")");
    }

    /** @brief 对象/数组嵌套深度守卫；超过 max_depth 直接报错 */
    struct depth_guard {
        parser& p;
        explicit depth_guard(parser& p) : p(p) {
            if (p.depth >= p.max_depth) p.fail("maximum nesting depth exceeded");
            ++p.depth;
        }
        ~depth_guard() { --p.depth; }
    };
    void ws() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }
    char peek() {
        if (pos >= src.size()) fail("unexpected end of input");
        return src[pos];
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++pos;
    }
    void literal(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (pos >= src.size() || src[pos] != *p) fail("bad literal");
            ++pos;
        }
    }

    value parse_value() {
        ws();
        switch (char c = peek()) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return value(parse_string());
        case 't': literal("true");  return value(true);
        case 'f': literal("false"); return value(false);
        case 'n': literal("null");  return value(nullptr);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
            fail("unexpected character");
        }
    }

    std::uint32_t hex4() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = peek(); ++pos;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }
    static void utf8_append(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos >= src.size()) fail("unterminated string");
            char c = src[pos++];
            if (c == '"') break;
            if (c != '\\') { out.push_back(c); continue; }
            char e = peek(); ++pos;
            switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {          // 高代理 → 配对
                    if (pos + 1 >= src.size() || src[pos] != '\\'
                        || src[pos + 1] != 'u') fail("missing low surrogate");
                    pos += 2;
                    std::uint32_t lo = hex4();
                    if (lo < 0xDC00 || lo > 0xDFFF) fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8_append(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
        return out;
    }

    value parse_number() {
        std::size_t start = pos;
        bool is_float = false;
        if (peek() == '-') ++pos;
        while (pos < src.size()) {
            char c = src[pos];
            if (c >= '0' && c <= '9') ++pos;
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_float = true; ++pos;
            } else break;
        }
        std::string_view num = src.substr(start, pos - start);
        if (!is_float) {
            std::int64_t i{};
            auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), i);
            if (ec == std::errc{} && p == num.data() + num.size()) return value(i);
        }
        double d{};
        auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), d);
        if (ec != std::errc{} || p != num.data() + num.size()) fail("bad number");
        return value(d);
    }

    value parse_array() {
        depth_guard guard{*this};
        expect('[');
        array out;
        ws();
        if (peek() == ']') { ++pos; return value(std::move(out)); }
        while (true) {
            out.push_back(parse_value());
            ws();
            char c = peek(); ++pos;
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']'");
        }
        return value(std::move(out));
    }

    value parse_object() {
        depth_guard guard{*this};
        expect('{');
        object out;
        ws();
        if (peek() == '}') { ++pos; return value(std::move(out)); }
        while (true) {
            ws();
            std::string key = parse_string();
            ws();
            expect(':');
            out.emplace_back(std::move(key), parse_value());
            ws();
            char c = peek(); ++pos;
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}'");
        }
        return value(std::move(out));
    }
};

// ── 序列化辅助 ──────────────────────────────────────────────────────────

void escape_to(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else out.push_back(char(c));
        }
    }
    out.push_back('"');
}

void dump_to(std::string& out, const value& v) {
    if (v.is_null()) out += "null";
    else if (v.is_bool()) out += v.as_bool() ? "true" : "false";
    else if (v.is_int()) out += std::to_string(v.as_int());
    else if (v.is_double()) {
        // 最短可往返形态：精度自 15 收缩至 17
        char buf[32];
        for (int prec : {15, 16, 17}) {
            std::snprintf(buf, sizeof buf, "%.*g", prec, v.as_double());
            if (std::strtod(buf, nullptr) == v.as_double()) break;
        }
        out += buf;
    }
    else if (v.is_string()) escape_to(out, v.as_string());
    else if (v.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& e : v.as_array()) {
            if (!first) out.push_back(',');
            first = false;
            dump_to(out, e);
        }
        out.push_back(']');
    }
    else if (v.is_object()) {
        out.push_back('{');
        bool first = true;
        for (const auto& [k, e] : v.as_object()) {
            if (!first) out.push_back(',');
            first = false;
            escape_to(out, k);
            out.push_back(':');
            dump_to(out, e);
        }
        out.push_back('}');
    }
}

} // namespace detail

value parse(std::string_view src) {
    return parse(src, {});
}

value parse(std::string_view src, const parse_options& opts) {
    detail::parser p{src, 0, opts.max_depth, 0};
    value v = p.parse_value();
    p.ws();
    if (p.pos != src.size()) p.fail("trailing characters");
    return v;
}

std::string dump(const value& v) {
    std::string out;
    detail::dump_to(out, v);
    return out;
}

} // namespace json
