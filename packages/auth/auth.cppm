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

/** @brief 路由要求角色；可重复标注，全部满足才放行 */
struct RequireRole {
    bean_name role{};
};

/** @brief 路由要求 scope；可重复标注，全部满足才放行 */
struct RequireScope {
    bean_name scope{};
};

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

/** @brief 生成 32 字节随机 opaque token（hex）；供各 TokenStore 使用 */
std::string new_token();

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

/** @brief 单条路由的权限要求 */
struct route_policy {
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
};

/**
 * @brief 启动期扫描出的路由权限表
 *
 * key 为 `bean类型名.handler方法名`，由 auth_policy_hook 在 context 构造
 * 完成后填充；请求期只读，不需要 web 模块知道 auth 注解。
 */
class [[=Bean{}]] AuthPolicy {
public:
    void set(std::string key, route_policy policy) {
        policies_.insert_or_assign(std::move(key), std::move(policy));
    }

    const route_policy* find(std::string_view key) const {
        const auto it = policies_.find(std::string(key));
        return it == policies_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, route_policy> policies_;
};

namespace detail {

template <class B>
consteval bool has_class_policy() {
    return std::meta::annotations_of_with_type(^^B, ^^RequireRole).size() > 0
        || std::meta::annotations_of_with_type(^^B, ^^RequireScope).size() > 0;
}

template <class B>
consteval bool is_policy_method(std::meta::info m) {
    if (!std::meta::is_function(m) || !std::meta::is_class_member(m)
        || std::meta::is_static_member(m) || !std::meta::is_public(m))
        return false;
    // 类级策略适用于该 bean 的所有 public 成员函数（实际只对路由生效）
    if (has_class_policy<B>()) return true;
    return std::meta::annotations_of_with_type(m, ^^RequireRole).size() > 0
        || std::meta::annotations_of_with_type(m, ^^RequireScope).size() > 0;
}

template <class B>
consteval std::size_t policy_method_count() {
    std::size_t n = 0;
    for (auto m : std::meta::members_of(^^B, std::meta::access_context::unchecked()))
        if (is_policy_method<B>(m)) ++n;
    return n;
}

template <class B, std::size_t I>
consteval std::meta::info nth_policy_method() {
    std::size_t seen = 0;
    for (auto m : std::meta::members_of(^^B, std::meta::access_context::unchecked())) {
        if (!is_policy_method<B>(m)) continue;
        if (seen == I) return m;
        ++seen;
    }
    throw "cboot.auth: policy method index out of range";
}

template <class B>
consteval std::size_t class_role_count() {
    return std::meta::annotations_of_with_type(^^B, ^^RequireRole).size();
}

template <class B, std::size_t K>
consteval bean_name class_role_at() {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(^^B, ^^RequireRole)) {
        if (seen == K) return std::meta::extract<RequireRole>(a).role;
        ++seen;
    }
    return {};
}

template <class B, std::size_t I>
consteval std::size_t role_count() {
    return std::meta::annotations_of_with_type(
        nth_policy_method<B, I>(), ^^RequireRole).size();
}

template <class B, std::size_t I, std::size_t K>
consteval bean_name role_at() {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(
             nth_policy_method<B, I>(), ^^RequireRole)) {
        if (seen == K) return std::meta::extract<RequireRole>(a).role;
        ++seen;
    }
    return {};
}

template <class B>
consteval std::size_t class_scope_count() {
    return std::meta::annotations_of_with_type(^^B, ^^RequireScope).size();
}

template <class B, std::size_t K>
consteval bean_name class_scope_at() {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(^^B, ^^RequireScope)) {
        if (seen == K) return std::meta::extract<RequireScope>(a).scope;
        ++seen;
    }
    return {};
}

template <class B, std::size_t I>
consteval std::size_t scope_count() {
    return std::meta::annotations_of_with_type(
        nth_policy_method<B, I>(), ^^RequireScope).size();
}

template <class B, std::size_t I, std::size_t K>
consteval bean_name scope_at() {
    std::size_t seen = 0;
    for (auto a : std::meta::annotations_of_with_type(
             nth_policy_method<B, I>(), ^^RequireScope)) {
        if (seen == K) return std::meta::extract<RequireScope>(a).scope;
        ++seen;
    }
    return {};
}

template <class B>
consteval std::string_view policy_bean_name() {
    if (std::meta::has_identifier(^^B)) return std::meta::identifier_of(^^B);
    return {};
}

template <class B, std::size_t I>
consteval std::string_view policy_method_name() {
    constexpr std::meta::info m = nth_policy_method<B, I>();
    if (std::meta::has_identifier(m)) return std::meta::identifier_of(m);
    return {};
}

template <class B, std::size_t I>
inline void add_policy(AuthPolicy& policy) {
    route_policy rp;

    const auto add_unique = [](std::vector<std::string>& v, std::string_view s) {
        if (std::find(v.begin(), v.end(), s) == v.end())
            v.emplace_back(s);
    };

    // 类级要求作为最低要求
    [&]<std::size_t... K>(std::index_sequence<K...>) {
        (add_unique(rp.roles, class_role_at<B, K>().view()), ...);
    }(std::make_index_sequence<class_role_count<B>()>{});
    [&]<std::size_t... K>(std::index_sequence<K...>) {
        (add_unique(rp.scopes, class_scope_at<B, K>().view()), ...);
    }(std::make_index_sequence<class_scope_count<B>()>{});

    // 方法级要求追加
    [&]<std::size_t... K>(std::index_sequence<K...>) {
        (add_unique(rp.roles, role_at<B, I, K>().view()), ...);
    }(std::make_index_sequence<role_count<B, I>()>{});
    [&]<std::size_t... K>(std::index_sequence<K...>) {
        (add_unique(rp.scopes, scope_at<B, I, K>().view()), ...);
    }(std::make_index_sequence<scope_count<B, I>()>{});

    const std::string bean(policy_bean_name<B>());
    const std::string method(policy_method_name<B, I>());
    if (bean.empty() || method.empty()) return;
    policy.set(bean + "." + method, std::move(rp));
}

template <class B>
inline void fill_policy(AuthPolicy& policy) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (add_policy<B, I>(policy), ...);
    }(std::make_index_sequence<policy_method_count<B>()>{});
}

} // namespace detail

/**
 * @brief 启动期扫描 Bean 方法上的 RequireRole/RequireScope
 *
 * 在 context 构造完成后填充 AuthPolicy；AuthGuard 请求期只查表。
 * 不依赖 web 路由表新增字段，也不反向依赖 auth。
 */
struct auth_policy_hook {
    template <class Ctx>
    struct instance {
        explicit instance(Ctx& ctx) {
            auto& policy = ctx.template get<AuthPolicy>();
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (detail::fill_policy<typename Ctx::template bean_type<I>>(policy),
                 ...);
            }(std::make_index_sequence<Ctx::bean_count>{});
        }
    };
};

/** @brief 授权失败事件 */
struct authorization_failed {
    std::string subject;
    std::string method;
    std::string path;
    std::string reason;
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
    AuthGuard(AuthService& service, AuthProps& props, AuthPolicy& policy,
              event_publisher& bus);

    Response handle(web::request_context& ctx, const web::next_fn& next);

private:
    bool is_public(const web::route_info& route) const;
    bool requires_auth(const web::route_info& route) const;
    bool allows(const web::route_info& route, const session& s) const;
    static bool matches(std::string_view pattern, std::string_view route_pattern,
                        std::string_view method);

    AuthService& service_;
    AuthProps& props_;
    AuthPolicy& policy_;
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
    using beans = type_list<auth::AuthProps, auth::AuthPolicy,
                            auth::AuthService, auth::AuthGuard>;
    using hooks = type_list<auth::auth_policy_hook>;
    using dependencies = type_list<>;
};

/**
 * @brief 进程内 TokenStore 组件
 *
 * 开发/单实例使用；应用选择本组件而不是 auth_component 的裸核心。
 * 多实例/持久化改选 cboot.auth.postgres 的 auth_postgres_component。
 */
export struct auth_memory_component {
    static constexpr bean_name name{"auth.memory"};
    using beans = type_list<auth::MemoryTokenStore>;
    using dependencies = type_list<auth_component>;
};
