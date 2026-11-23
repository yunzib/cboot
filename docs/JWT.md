# JWT 校验

`cboot.jwt` 是独立的 JWT 校验组件，只做 resource server 侧 token 校验：

- HS256
- RS256
- `exp` / `nbf` / `iss` / `aud`
- `sub`
- 可配置 `roles` / `scope` claim

它不管理登录态、不签发 token、不做踢人/refresh/吊销。登录态由
[认证](认证.md) 组件负责；两者可以按需组合。

## 1. 配置

```cpp
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
```

前缀固定为 `jwt`。

### HS256

```json
{
  "jwt": {
    "algorithm": "HS256",
    "secret": "${JWT_SECRET}",
    "issuer": "https://issuer.example",
    "audience": "cboot-api",
    "leeway_s": 30
  }
}
```

`secret` 为空或未展开的 `${JWT_SECRET}` 时回退 `JWT_SECRET` 环境变量。

### RS256

```json
{
  "jwt": {
    "algorithm": "RS256",
    "public_key_path": "secrets/jwt-public.pem",
    "issuer": "https://issuer.example",
    "audience": "cboot-api"
  }
}
```

`public_key_path` 优先；也可直接写 PEM 到 `public_key`。

## 2. 直接使用

```cpp
import cboot.jwt;

jwt::JwtVerifier verifier(props);
try {
    jwt::claims c = verifier.verify(token);
    std::string subject = c.subject;
    auto roles = c.roles;
    auto scopes = c.scopes;
} catch (const jwt::error&) {
    // 无效
}
```

CMake：

```cmake
find_package(cboot.jwt REQUIRED)
target_link_libraries(app PRIVATE cboot::jwt)
```

非系统 OpenSSL：

```bash
cmake -DOPENSSL_ROOT_DIR=C:/path/to/openssl ...
```

Windows 运行时需要 `libcrypto-3-x64.dll`。

## 3. 校验规则

- header `alg` 必须与配置完全一致
- 拒绝 `alg=none`
- 拒绝 `crit` header
- `exp` 默认必填；过期拒绝
- `nbf` 存在时检查
- `issuer` / `audience` 配置后必须匹配
- `roles` / `scope` claim 可配置
- 签名验证在 claims 检查之前

## 4. 与 Web 集成

`cboot.jwt` 不提供全局中间件。需要 JWT 守卫时：

- 自己写 `[[=web::Middleware{}]]`，注入 `JwtVerifier&`
- 校验成功后把 principal 写入 `request_context`
- 失败返回 401

示例见 [自定义认证](自定义认证.md)。

如果使用服务端登录态，直接用 [认证](认证.md) 的 opaque token，
不需要 JWT。

## 5. 限制

- 不支持 ES256/RS512 等算法
- 不支持 JWKS 自动轮换
- 不签发 token
- 不保存 refresh/吊销状态
- 不提供登录接口

相关文档：[认证](认证.md)、[自定义认证](自定义认证.md)。
