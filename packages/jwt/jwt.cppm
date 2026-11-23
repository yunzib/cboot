/**
 * @file jwt.cppm
 * @brief JWT 校验 — compact JWS 解析、签名验证与 claims 检查
 *
 * 只做 resource server 侧校验：不签发、不刷新、不维护用户存储。
 * 支持 HS256 / RS256；算法与配置必须完全一致，拒绝 none。
 *
 * OpenSSL 头文件只在 src/jwt.cpp 出现；模块接口不暴露 OpenSSL 类型。
 */
export module cboot.jwt;

import std;
import cboot.core.annotations;   // Config / Bean / bean_name
import cboot.meta.reflect;       // type_list
import cboot.json;

export namespace jwt {

/**
 * @brief JWT 校验配置（前缀 jwt）
 *
 * HMAC：
 *   - algorithm = "HS256"
 *   - secret 必填；空或 "${JWT_SECRET}" 未展开时回退 JWT_SECRET 环境变量
 *
 * RSA：
 *   - algorithm = "RS256"
 *   - public_key_path 指向 PEM 公钥文件，或直接写 public_key
 *
 * issuer / audience 为空时不校验对应 claim。
 * 密码/密钥不会写入错误信息或日志。
 */
struct [[=Config{"jwt"}]] JwtProps {
    std::string algorithm{"HS256"};
    std::string secret;
    std::string public_key;
    std::string public_key_path;
    std::string issuer;
    std::string audience;
    std::string roles_claim{"roles"};
    std::string scope_claim{"scope"};
    int leeway_s{30};
    bool require_exp{true};
};

/** @brief 校验成功后的 JWT 声明（只保留常用字段 + 原始 payload） */
struct claims {
    std::string subject;
    std::string issuer;
    std::vector<std::string> audience;
    std::vector<std::string> roles;
    std::vector<std::string> scopes;
    std::int64_t expires_at{};
    json::value payload;
};

/** @brief JWT 校验错误（签名、格式、过期、claim 不匹配） */
class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief JWT 校验器 Bean
 *
 * 构造时校验配置并加载公钥/密钥；verify() 只读，可并发调用。
 */
class [[=Bean{}]] JwtVerifier {
public:
    explicit JwtVerifier(JwtProps& props);
    ~JwtVerifier();

    /** @brief 校验 compact JWT；失败抛 jwt::error */
    claims verify(std::string_view token) const;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace jwt

/**
 * @brief JWT 校验契约组件
 *
 * 只贡献配置与校验器；认证/路由守卫由 cboot.auth 组件依赖本组件。
 */
export struct jwt_component {
    static constexpr bean_name name{"jwt"};
    using beans = type_list<jwt::JwtProps, jwt::JwtVerifier>;
    using dependencies = type_list<>;
};
