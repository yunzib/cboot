/**
 * @file auth.cpp
 * @brief auth 实现 — opaque token、内存会话、路由策略、认证事件
 */
module;

#if defined(_WIN32)
// GCC16 模块边界守卫：Windows 头会拉入 C 头，与 import std 冲突。
#define _GLIBCXX_INCLUDE_NEXT_C_HEADERS 1
#define _MM_MALLOC_H_INCLUDED
#define _X86INTRIN_H_INCLUDED
#define _IMMINTRIN_H_INCLUDED
#define _XMMINTRIN_H_INCLUDED
#define _EMMINTRIN_H_INCLUDED
#define _MMINTRIN_H_INCLUDED
#define _PMMINTRIN_H_INCLUDED
#define _CPUID_H_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

module cboot.auth;

import std;
import cboot.web.http;
import cboot.web.middleware;

namespace auth {

namespace {

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string random_token() {
    std::array<unsigned char, 32> bytes{};
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, bytes.data(),
                        static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("cboot.auth: random token generation failed");
#else
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const auto n = ::getrandom(bytes.data() + filled,
                                   bytes.size() - filled, 0);
        if (n <= 0)
            throw std::runtime_error("cboot.auth: random token generation failed");
        filled += static_cast<std::size_t>(n);
    }
#endif

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

/** @brief 读取 Cookie 值；未命中 nullopt（值不 URL 解码） */
std::optional<std::string_view> cookie_value(const Request& request,
                                             std::string_view name) {
    const auto header = request.header("Cookie");
    if (!header) return std::nullopt;

    std::string_view rest = *header;
    while (!rest.empty()) {
        const std::size_t semi = rest.find(';');
        std::string_view item = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{}
                                              : rest.substr(semi + 1);
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) continue;
        if (trim(item.substr(0, eq)) == name)
            return trim(item.substr(eq + 1));
    }
    return std::nullopt;
}

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        std::size_t end = path.find('/', i);
        if (end == std::string_view::npos) end = path.size();
        out.push_back(path.substr(i, end - i));
        i = end;
    }
    return out;
}

bool match_segments(const std::vector<std::string_view>& pattern,
                    const std::vector<std::string_view>& path,
                    std::size_t pi, std::size_t si) {
    if (pi == pattern.size()) return si == path.size();

    if (pattern[pi] == "**") {
        for (std::size_t k = si; k <= path.size(); ++k)
            if (match_segments(pattern, path, pi + 1, k)) return true;
        return false;
    }

    if (si == path.size()) return false;
    if (pattern[pi] != "*" && pattern[pi] != path[si]) return false;
    return match_segments(pattern, path, pi + 1, si + 1);
}

Response unauthorized() {
    json::object body;
    body.emplace_back("error", json::value("unauthorized"));
    Response r = json_response(json::value(std::move(body)));
    r.status = 401;
    r.header("WWW-Authenticate", "Bearer");
    return r;
}

Response forbidden() {
    json::object body;
    body.emplace_back("error", json::value("forbidden"));
    Response r = json_response(json::value(std::move(body)));
    r.status = 403;
    return r;
}

} // namespace

std::string new_token() { return random_token(); }

// ==================== MemoryTokenStore ====================

struct MemoryTokenStore::impl {
    AuthProps* props = nullptr;
    mutable std::mutex mu;
    std::unordered_map<std::string, session> tokens;

    std::int64_t expiry_for(const session& s) const {
        if (s.expires_at > 0) return s.expires_at;
        const int timeout = props->timeout_s > 0 ? props->timeout_s : 7200;
        return s.issued_at + timeout;
    }
};

MemoryTokenStore::MemoryTokenStore(AuthProps& props)
    : impl_(std::make_unique<impl>()) {
    impl_->props = &props;
}

MemoryTokenStore::~MemoryTokenStore() = default;

std::string MemoryTokenStore::create(session s) {
    if (s.issued_at <= 0) s.issued_at = now_seconds();
    if (s.expires_at <= 0) s.expires_at = impl_->expiry_for(s);
    std::string token;
    {
        std::lock_guard lock(impl_->mu);
        do {
            token = new_token();
        } while (impl_->tokens.contains(token));
        impl_->tokens.emplace(token, std::move(s));
    }
    return token;
}

std::optional<session> MemoryTokenStore::get(std::string_view token) const {
    std::lock_guard lock(impl_->mu);
    auto it = impl_->tokens.find(std::string(token));
    if (it == impl_->tokens.end()) return std::nullopt;
    if (it->second.expires_at > 0 && now_seconds() > it->second.expires_at) {
        impl_->tokens.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void MemoryTokenStore::revoke(std::string_view token) {
    std::lock_guard lock(impl_->mu);
    impl_->tokens.erase(std::string(token));
}

void MemoryTokenStore::revoke_subject(std::string_view subject) {
    std::lock_guard lock(impl_->mu);
    for (auto it = impl_->tokens.begin(); it != impl_->tokens.end();) {
        if (it->second.subject == subject) it = impl_->tokens.erase(it);
        else ++it;
    }
}

bool MemoryTokenStore::touch(std::string_view token, std::int64_t expires_at) {
    std::lock_guard lock(impl_->mu);
    auto it = impl_->tokens.find(std::string(token));
    if (it == impl_->tokens.end()) return false;
    if (it->second.expires_at > 0 && now_seconds() > it->second.expires_at) {
        impl_->tokens.erase(it);
        return false;
    }
    it->second.expires_at = expires_at;
    return true;
}

// ==================== AuthService ====================

AuthService::AuthService(TokenStore& store, AuthProps& props)
    : store_(store), props_(props) {}

std::string AuthService::login(session s) {
    if (s.issued_at <= 0) s.issued_at = now_seconds();
    if (s.expires_at <= 0) {
        const int timeout = props_.timeout_s > 0 ? props_.timeout_s : 7200;
        s.expires_at = s.issued_at + timeout;
    }
    return store_.create(std::move(s));
}

std::optional<std::string_view> AuthService::token(
    const web::request_context& ctx) const {
    if (const auto header = ctx.request.header(props_.token_header)) {
        std::string_view value = trim(*header);
        if (props_.token_prefix.empty()) {
            return value.empty() ? std::nullopt
                                 : std::optional<std::string_view>(value);
        }

        if (value.size() < props_.token_prefix.size()) return std::nullopt;
        if (!iequals(value.substr(0, props_.token_prefix.size()),
                     props_.token_prefix))
            return std::nullopt;
        value = trim(value.substr(props_.token_prefix.size()));
        return value.empty() ? std::nullopt
                             : std::optional<std::string_view>(value);
    }

    // header 缺失才回退 Cookie；header 存在但格式错误时不混用两种来源。
    if (!props_.token_cookie.empty())
        return cookie_value(ctx.request, props_.token_cookie);
    return std::nullopt;
}

std::optional<session> AuthService::find(std::string_view token) const {
    if (token.empty()) return std::nullopt;
    return store_.get(token);
}

std::optional<session> AuthService::current(
    const web::request_context& ctx) const {
    const auto t = token(ctx);
    if (!t) return std::nullopt;
    auto s = find(*t);
    if (!s) return std::nullopt;

    // 滑动续期：活跃请求延长空闲过期，但不超过 issued_at + timeout_s。
    if (props_.active_timeout_s > 0) {
        const std::int64_t idle = now_seconds() + props_.active_timeout_s;
        std::int64_t cap = 0;
        if (s->issued_at > 0 && props_.timeout_s > 0)
            cap = s->issued_at + props_.timeout_s;
        const std::int64_t next = (cap > 0 && idle > cap) ? cap : idle;
        if (next > s->expires_at && store_.touch(*t, next))
            s->expires_at = next;
    }
    return s;
}

void AuthService::logout(const web::request_context& ctx) {
    if (const auto t = token(ctx)) store_.revoke(*t);
}

void AuthService::logout(std::string_view token) {
    if (!token.empty()) store_.revoke(token);
}

void AuthService::kickout(std::string_view subject) {
    store_.revoke_subject(subject);
}

void AuthService::set_cookie(Response& response, std::string_view value,
                             std::int64_t max_age) const {
    if (props_.token_cookie.empty()) return;

    const std::string_view path =
        props_.cookie_path.empty() ? std::string_view("/")
                                   : std::string_view(props_.cookie_path);
    const std::string_view same_site =
        props_.cookie_same_site.empty() ? std::string_view("Lax")
                                        : std::string_view(props_.cookie_same_site);

    std::string v;
    v.reserve(props_.token_cookie.size() + value.size() + path.size()
              + same_site.size() + 48);
    v += props_.token_cookie;
    v += '=';
    v += value;
    v += "; Path=";
    v += path;
    v += "; Max-Age=";
    v += std::to_string(max_age);
    if (props_.cookie_http_only) v += "; HttpOnly";
    if (props_.cookie_secure) v += "; Secure";
    v += "; SameSite=";
    v += same_site;
    response.header("Set-Cookie", std::move(v));
}

void AuthService::attach_cookie(Response& response,
                                std::string_view token) const {
    const std::int64_t max_age =
        props_.timeout_s > 0 ? props_.timeout_s : 7200;
    set_cookie(response, token, max_age);
}

void AuthService::clear_cookie(Response& response) const {
    set_cookie(response, {}, 0);
}

// ==================== AuthGuard ====================

AuthGuard::AuthGuard(AuthService& service, AuthProps& props,
                     AuthPolicy& policy, event_publisher& bus)
    : service_(service), props_(props), policy_(policy), bus_(bus) {
    if (props_.default_policy != "protected"
        && props_.default_policy != "public")
        throw std::invalid_argument(
            "cboot.auth: default_policy must be 'protected' or 'public'");
}

bool AuthGuard::matches(std::string_view pattern, std::string_view path,
                        std::string_view method) {
    std::string_view p = trim(pattern);
    if (p.empty()) return false;

    // 仅当 ':' 出现在第一个 '/' 之前，才把前缀当 HTTP 方法。
    const std::size_t colon = p.find(':');
    const std::size_t slash = p.find('/');
    if (colon != std::string_view::npos
        && (slash == std::string_view::npos || colon < slash)) {
        const std::string_view m = p.substr(0, colon);
        if (m != "*" && !iequals(m, method)) return false;
        p = p.substr(colon + 1);
    }

    if (const std::size_t q = p.find('?'); q != std::string_view::npos)
        p = p.substr(0, q);

    const auto pat = split_path(p);
    const auto actual = split_path(path);
    return match_segments(pat, actual, 0, 0);
}

bool AuthGuard::is_public(const web::route_info& route) const {
    const std::string_view method = method_name(route.method);
    for (const auto& pattern : props_.public_paths) {
        // route.pattern 是注册 pattern；policy 使用实际 path 时由调用方传入。
        if (matches(pattern, route.pattern, method)) return true;
    }
    return false;
}

bool AuthGuard::requires_auth(const web::route_info& route) const {
    const std::string_view method = method_name(route.method);
    for (const auto& pattern : props_.protected_paths)
        if (matches(pattern, route.pattern, method)) return true;
    return props_.default_policy == "protected";
}

bool AuthGuard::allows(const web::route_info& route, const session& s) const {
    const std::string key = std::string(route.bean) + "." + std::string(route.handler);
    const route_policy* p = policy_.find(key);
    if (!p) return true;

    for (const auto& role : p->roles) {
        if (std::find(s.roles.begin(), s.roles.end(), role) == s.roles.end())
            return false;
    }
    for (const auto& scope : p->scopes) {
        if (std::find(s.scopes.begin(), s.scopes.end(), scope) == s.scopes.end())
            return false;
    }
    return true;
}

Response AuthGuard::handle(web::request_context& ctx, const web::next_fn& next) {
    // 404/OPTIONS 不触发认证；OPTIONS 由 CORS/全局中间件处理。
    if (!ctx.route || ctx.request.method == "OPTIONS") return next();

    // 路由 pattern 与实际 path：policy 表达式按注册 pattern 匹配，
    // 这样 /users/{id} 这类参数化路由不会被具体值绕过。
    if (is_public(*ctx.route)) return next();
    if (!requires_auth(*ctx.route)) return next();

    auto s = service_.current(ctx);
    if (!s) {
        bus_.publish(authentication_failed{
            .method = std::string(ctx.request.method),
            .path = std::string(ctx.request.path),
            .reason = "missing or invalid token",
        });
        return unauthorized();
    }

    if (!allows(*ctx.route, *s)) {
        bus_.publish(authorization_failed{
            .subject = s->subject,
            .method = std::string(ctx.request.method),
            .path = std::string(ctx.request.path),
            .reason = "missing required role or scope",
        });
        return forbidden();
    }

    bus_.publish(authentication_succeeded{
        .subject = s->subject,
        .method = std::string(ctx.request.method),
        .path = std::string(ctx.request.path),
    });
    ctx.put(std::move(*s));
    return next();
}

} // namespace auth
