# 全栈待办示例（PostgreSQL + 原生 HTML）

一个可直接运行的最小完整项目：

- 后端：cboot 编译期路由 + PostgreSQL ORM + 登录认证（`cboot.auth`）
- 前端：单文件 `web/index.html`，原生 `fetch`，无构建步骤
- 分层：`model → repository → service → api → app`，`main.cpp` 只启动
- 数据库：建表 + 示例数据备份在 `db/`，其他人可直接恢复

```text
examples/todo/
├── main.cpp                 仅 main + App::run
├── src/
│   ├── model.cppm           Task / User / DTO
│   ├── security.cppm/.cpp   PBKDF2-HMAC-SHA256 密码哈希
│   ├── repository.cppm      EntityManager 数据访问
│   ├── service.cppm         TaskService / AccountService
│   ├── setup.cppm           PostConstruct 建表 + 默认账号
│   ├── api/auth_api.cppm    登录 / 登出 / me
│   ├── api/errors.cppm      自定义 401/403 错误码（ErrorMapper）
│   ├── api/task_api.cppm    REST 接口（受 AuthGuard 保护）
│   ├── api/page_api.cppm    返回 web/index.html
│   └── app.cppm             App 与组件选择
├── web/index.html           前端页面（登录 + 待办）
├── app.json                 端口、数据库与 auth 配置
└── db/
    ├── schema.sql           仅建表（空库初始化）
    └── backup.sql           建表 + 示例数据
```

## 1. 准备 PostgreSQL

需要 PostgreSQL 14+（示例使用 18 验证）。创建数据库：

```bash
createdb -U postgres cboot_demo
```

或：

```sql
CREATE DATABASE cboot_demo;
```

已有 `db/schema.sql` / `db/backup.sql` 时也可以直接恢复，见第 7 节。

## 2. 配置密码

`app.json` 默认：

```json
"password": "${PGPASSWORD}"
```

任选一种：

1. 环境变量：

```cmd
set PGPASSWORD=你的密码
```

PowerShell：

```powershell
$env:PGPASSWORD = "你的密码"
```

2. 直接写进 `app.json`（不要提交真实密码）：

```json
"password": "你的密码"
```

3. 删除 `password` 字段，让 libpq 自己读取 `PGPASSWORD` / `pgpass.conf`。

`${PGPASSWORD}` 未展开时按空密码处理，libpq 会继续尝试环境变量或 pgpass。

## 3. 安装 cboot SDK

在 cboot 仓库根目录：

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release
cmake --install build-release --prefix build-release/install
```

## 4. 构建示例

```bash
cmake -S examples/todo -B examples/todo/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build-release/install"
cmake --build examples/todo/build
```

如果 `pg_config` 不在 PATH，追加：

```bash
  -DPostgreSQL_ROOT="C:/SQL_DB/PostgreSQL/18" \
  -DPostgreSQL_LIBRARY_RELEASE="C:/SQL_DB/PostgreSQL/18/lib/libpq.lib" \
  -DPostgreSQL_LIBRARY_DEBUG="C:/SQL_DB/PostgreSQL/18/lib/libpq.lib"
```

Windows 运行时需要 `libpq.dll` 在 PATH 或 exe 同目录；PostgreSQL 的 `bin`
目录通常已经在 PATH。

密码哈希使用 OpenSSL，CMake 会自动查找并链接 `OpenSSL::Crypto`；
MinGW 编译器自带的 `opt` 目录会被优先使用，`libcrypto*.dll` 会复制到
exe 同目录。

## 5. 运行

`app.json` 与 `web/index.html` 都按工作目录查找，因此从示例目录启动：

```bash
cd examples/todo
./build/cboot_example_todo.exe
```

浏览器打开：

```text
http://127.0.0.1:8090
```

不要直接双击 `web/index.html`，也不要用 Live Server 等其他端口打开；
页面和 API 必须同源，否则浏览器会按 CORS 拦截登录请求。

启动时会：

1. 加载 `app.json`
2. 打开 PostgreSQL 连接池
3. `Setup` 的 `PostConstruct` 创建任务表、用户表、唯一索引，
   并在用户表为空时创建默认账号 `admin / admin123`
4. 挂载认证路由、`/api/tasks` 与 `/` 路由

数据库不可达时启动失败，属于预期行为。默认账号仅用于本地演示，
生产环境必须删除或改密。

## 6. 登录与接口

浏览器打开页面后先登录，登录态是 opaque token，保存在
`localStorage`，后续请求自动带 `Authorization: Bearer <token>`。
后端同时下发 `cboot_token` Cookie（`HttpOnly` + `SameSite=Lax`），
两种传输都支持。

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/api/auth/login` | 登录，body `{"username":"admin","password":"admin123"}` |
| `POST` | `/api/auth/logout` | 登出并清除 Cookie |
| `GET` | `/api/auth/me` | 当前用户 |
| `GET` | `/api/tasks` | 当前用户的待办列表 |
| `POST` | `/api/tasks` | 创建，body `{"title":"..."}` |
| `PATCH` | `/api/tasks/{id}` | 更新完成状态，body `{"done":true}` |
| `DELETE` | `/api/tasks/{id}` | 删除 |
| `GET` | `/`、`/index.html` | 前端页面 |

- `/api/auth/login`、`/`、`/index.html`、`/health` 公开
- 其余 `/api/**` 由 `AuthGuard` 保护，未登录返回 401
- 错误响应由全局 `ErrorHandler` 统一生成；示例的 `ApiErrors`
  （`[[=web::ErrorMapper]]`）给 401/403 附加机器可读 `code`
- 每个用户只能看到/修改自己的待办，越权返回 404
- `POST` 的 `title` 使用 `NotBlank` 校验，空白返回 400
- 密码使用 PBKDF2-HMAC-SHA256（12 万次迭代、随机盐）存储

```bash
# 登录并取 token（jq 可选，这里用 sed）
TOKEN=$(curl -s -X POST -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"admin123"}' \
  http://127.0.0.1:8090/api/auth/login |
  sed -n 's/.*"token":"\([^"]*\)".*/\1/p')

curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8090/api/auth/me

curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"title":"写文档"}' http://127.0.0.1:8090/api/tasks

curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8090/api/tasks

curl -X PATCH -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"done":true}' http://127.0.0.1:8090/api/tasks/1

curl -X DELETE -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8090/api/tasks/1
```

未登录或密码错误的响应由 `ApiErrors` 统一为：

```json
{"error":"unauthorized","code":"auth.unauthorized"}
```

`code` 区分 `auth.unauthorized`、`auth.invalid_credentials`、
`auth.forbidden`；其余错误返回 `nullopt` 回退默认响应。实现见
`src/api/errors.cppm`，机制见 [全局错误处理](../../docs/错误处理.md)。

## 7. 备份与恢复

`db/schema.sql`：只有建表语句，适合初始化空库。  
`db/backup.sql`：建表 + 示例数据（用户与待办），适合直接还原演示环境。
备份不包含 `cboot_auth_sessions` 的会话数据（避免把有效 token 带进仓库）。

恢复：

```bash
createdb -U postgres cboot_demo
psql -U postgres -d cboot_demo -f db/schema.sql
# 或带示例数据：
psql -U postgres -d cboot_demo -f db/backup.sql
```

两个文件都使用 `--no-owner --no-privileges` 导出，不绑定本机用户名。
`backup.sql` 里的默认账号仍是 `admin / admin123`（PBKDF2 哈希）。

重新导出：

```bash
pg_dump -U postgres --no-owner --no-privileges \
  --schema-only cboot_demo > db/schema.sql

pg_dump -U postgres --no-owner --no-privileges --inserts \
  --exclude-table-data=cboot_auth_sessions \
  cboot_demo > db/backup.sql
```

## 8. 清理

```sql
DROP TABLE IF EXISTS public.cboot_demo_tasks;
DROP TABLE IF EXISTS public.cboot_demo_users;
DROP TABLE IF EXISTS public.cboot_auth_sessions;
-- 或
DROP DATABASE cboot_demo;
```

## 9. 分层说明

| 层 | 文件 | 职责 |
|---|---|---|
| model | `src/model.cppm` | 实体、DTO、校验注解 |
| security | `src/security.cppm/.cpp` | PBKDF2 密码哈希与校验 |
| repository | `src/repository.cppm` | 只做持久化，封装 EntityManager |
| service | `src/service.cppm` | 待办业务与账号登录，不依赖 HTTP |
| api | `src/api/*.cppm` | 请求/响应映射、状态码、取当前会话 |
| errors | `src/api/errors.cppm` | `ErrorMapper`，统一 401/403 错误码 |
| app | `src/app.cppm` | 组件选择、配置路径、端口 |
| entry | `main.cpp` | `main()` 只调用 `run(argc, argv)` |

依赖方向单向向下：api → service → repository → model。
`AuthGuard` 由 `auth_component` 提供，在请求进入 handler 前完成认证，
并把 `auth::session` 放入 `request_context`。handler 通过
`ctx.get<auth::session>()` 直接取会话，不重复判断 401；认证失败由
全局 `ErrorHandler` 统一处理。
生产项目应把 `Setup::create_schema` 换成迁移脚本，并替换默认账号。
