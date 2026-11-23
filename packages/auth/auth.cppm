/**
 * @file auth.cppm
 * @brief 认证契约组件 — 登录态、opaque token、路由策略与认证事件
 *
 * 这个组件管理服务端登录态，不负责用户存储或密码校验：
 *   - AuthService::login/logout/kickout
 *   - TokenStore 抽象；默认 MemoryTokenStore
 *   - AuthGuard：按 AuthProps 路由策略全局拦截，发布成功/失败事件
 *
 * JWT 校验在独立的 cboot.jwt 组件中，不在本组件内。
 */
export module cboot.auth;

import std;
import cboot.core.annotations;   // Bean / Config / bean_name
import cboot.core.context;       // event_publisher
import cboot.meta.reflect;       // type_list
import cboot.web.http;           // Request / Response
import cboot.web.middleware;     // Middleware / request_context / next_fn

export namespace auth {

/**
 * @brief 认证配置（前缀 auth）
 *
 * token_header/token_prefix 控制 token 提取；默认 Authorization: Bearer。
 * 路由策略：
 *   - public_paths 命中 → 放行
 *   - protected_paths 命中 → 认证
 *   - 都未命中 → default_policy（"protected" / "public"）
 */
struct [[=Config{"auth"}]] AuthProps {
    int timeout_s{7200};
    std::string token_header{"Authorization"};
    std::string token_prefix{"Bearer "};
    std::string default_policy{"protected"};
    std::vector<std::string> public_paths;
    std::vector<std::string> protected_paths;
};

/** @brief 登录态；认证成功后写入 request_context */
struct session {
    std::string subject;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
    std::int64_t expires_at{};
    std::string device;
};

/** @brief 兼容命名：principal 就是 session */
using principal = session;

/**
 * @brief token → session 存储抽象
 *
 * 默认实现是进程内 MemoryTokenStore；Redis/数据库实现可替换。
 * 实现必须线程安全。
 */
class TokenStore {
public:
    virtual ~TokenStore() = default;

    /** @brief 创建 token；expires_at 由调用方填好 */
    virtual std::string create(session s) = 0;

    /** @brief 查询；过期 token 返回 nullopt */
    virtual std::optional<session> get(std::string_view token) const = 0;

    /** @brief 撤销单个 token */
    virtual void revoke(std::string_view token) = 0;

    /** @brief 撤销某 subject 的全部 token（踢人） */
    virtual void revoke_subject(std::string_view subject) = 0;
};

/** @brief 进程内 token store；线程安全，惰性过期清理 */
class [[=Bean{}]] MemoryTokenStore final : public TokenStore {
public:
    explicit MemoryTokenStore(AuthProps& props);
    ~MemoryTokenStore() override;

    std::string create(session s) override;
    std::optional<session> get(std::string_view token) const override;
    void revoke(std::string_view token) override;
    void revoke_subject(std::string_view subject) override;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

/**
 * @brief 登录态服务
 *
 * 应用在登录接口中校验用户/密码后调用 login；本类不接触密码。
 */
class [[=Bean{}]] AuthService {
public:
    AuthService(TokenStore& store, AuthProps& props);

    /** @brief 创建登录态；expires_at 为 0 时使用 AuthProps.timeout_s */
    std::string login(session s);

    /** @brief 从请求头提取 token 并查询；失败 nullopt */
    std::optional<session> current(const web::request_context& ctx) const;

    /** @brief 直接按 token 查询 */
    std::optional<session> find(std::string_view token) const;

    /** @brief 撤销请求中的 token */
    void logout(const web::request_context& ctx);

    /** @brief 撤销指定 token */
    void logout(std::string_view token);

    /** @brief 踢掉 subject 的全部登录态 */
    void kickout(std::string_view subject);

    /** @brief 从请求提取 token（不查询）；无 token nullopt */
    std::optional<std::string_view> token(const web::request_context& ctx) const;

private:
    TokenStore& store_;
    AuthProps& props_;
};

/** @brief 认证失败事件；不包含 token/密码 */
struct authentication_failed {
    std::string method;
    std::string path;
    std::string reason;
};

/** @brief 认证成功事件 */
struct authentication_succeeded {
    std::string subject;
    std::string method;
    std::string path;
};

/**
 * @brief 全局认证守卫
 *
 * AuthProps 决定哪些路由公开/受保护；默认 protected。
 * 失败发布 authentication_failed 并返回 401；成功发布
 * authentication_succeeded 并把 auth::session 放入 request_context。
 */
class [[=Bean{}]] [[=web::Middleware{.order = 100}]] AuthGuard {
public:
    AuthGuard(AuthService& service, AuthProps& props, event_publisher& bus);

    Response handle(web::request_context& ctx, const web::next_fn& next);

private:
    bool is_public(const web::route_info& route) const;
    bool requires_auth(const web::route_info& route) const;
    static bool matches(std::string_view pattern, std::string_view route_pattern,
                        std::string_view method);

    AuthService& service_;
    AuthProps& props_;
    event_publisher& bus_;
};

} // namespace auth

/**
 * @brief 认证契约组件描述符
 *
 * 不依赖 cboot.jwt；登录态默认使用 opaque token。
 */
export struct auth_component {
    static constexpr bean_name name{"auth"};
    using beans = type_list<auth::AuthProps, auth::MemoryTokenStore,
                            auth::AuthService, auth::AuthGuard>;
    using dependencies = type_list<>;
};
