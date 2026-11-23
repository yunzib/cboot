# CORS

`cboot.web.cors` 是可选契约组件，提供全局 CORS 中间件：

- 来源白名单（支持 `*` 通配）
- 预检 `OPTIONS` 短路 204
- 方法 / 请求头校验
- 凭证、暴露头、`Max-Age`

不加入 `components` 时没有任何 CORS 行为。

## 1. 装配

```cpp
import cboot.app;
import cboot.web.cors;

struct App : application<App> {
    using components = type_list<cors_component>;
};
```

```cmake
target_link_libraries(app PRIVATE cboot::app cboot::web)
```

`cors_component` 贡献 `web::CorsProps` 与全局中间件 `web::Cors`
（`order = -1000`），在 `AuthGuard`（`order = 100`）之前执行。

## 2. 配置

```json
{
  "cors": {
    "enabled": true,
    "allowed_origins": [
      "https://app.example.com",
      "https://*.example.com"
    ],
    "allow_credentials": true,
    "allowed_methods": ["GET", "POST", "PUT", "PATCH", "DELETE"],
    "allowed_headers": ["Content-Type", "Authorization", "X-Request-Id"],
    "exposed_headers": ["X-Request-Id"],
    "max_age_s": 600
  }
}
```

| 键 | 默认 | 说明 |
|---|---|---|
| `enabled` | `true` | 关闭后完全直通，不处理 Origin |
| `allowed_origins` | `["*"]` | 完整 Origin 或通配模式 |
| `allow_credentials` | `false` | 是否允许浏览器携带 Cookie |
| `allowed_methods` | GET/POST/PUT/PATCH/DELETE/OPTIONS | 预检允许的方法；`*` 表示任意 |
| `allowed_headers` | `Content-Type`, `Authorization` | 预检允许的请求头；`*` 表示回显请求头 |
| `exposed_headers` | 空 | 浏览器可读的响应头 |
| `max_age_s` | `600` | 预检缓存秒数 |

来源匹配支持 `*`：

```text
*                         任意来源（不能与 allow_credentials 同时使用）
https://app.example.com   精确来源
https://*.example.com     任意子域
```

`allow_credentials = true` 且白名单含裸 `*` 时，组件构造失败
（浏览器不接受 `Access-Control-Allow-Origin: *` 携带凭证）。

## 3. 请求行为

| 请求 | 行为 |
|---|---|
| 无 `Origin` | 直通，不加 CORS 头 |
| Origin 不匹配 | 直通，不加 CORS 头（浏览器自行拦截） |
| 实际请求 | 执行后续链，响应附加 CORS 头 |
| 预检允许 | `204` 短路，不进入路由/AuthGuard |
| 预检方法或请求头不允许 | `403`，不加 CORS 头 |

实际请求响应：

```http
Access-Control-Allow-Origin: https://app.example.com
Access-Control-Allow-Credentials: true
Access-Control-Expose-Headers: X-Request-Id
Vary: Origin
```

预检响应：

```http
HTTP/1.1 204 No Content
Access-Control-Allow-Origin: https://app.example.com
Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, Authorization
Access-Control-Max-Age: 600
Vary: Origin, Access-Control-Request-Method, Access-Control-Request-Headers
```

白名单为裸 `*` 且未开启凭证时，`Access-Control-Allow-Origin` 输出 `*`；
其余情况回显实际 Origin。

## 4. Cookie 与凭证

浏览器跨域携带 Cookie 需要两侧同时配置：

```json
{
  "cors": {
    "allowed_origins": ["https://app.example.com"],
    "allow_credentials": true
  },
  "auth": {
    "token_cookie": "cboot_token",
    "cookie_same_site": "None",
    "cookie_secure": true
  }
}
```

- `SameSite=None` 必须配合 `Secure`（HTTPS）
- 开启凭证后不要使用裸 `*`
- 写操作仍需 CSRF 防护

## 5. 边界

- CORS 只限制浏览器跨域读取，不是认证/授权机制
- 非浏览器客户端不受 CORS 影响，仍需 `AuthGuard`
- 预检按配置放行，不校验路由是否存在
