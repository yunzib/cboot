# ORM 组件示例

通过 `find_package(cboot.orm.postgres)` 使用 PostgreSQL 驱动组件：

- 实体注解 `Table` / `Column` / `Id`
- `EntityManager` CRUD
- 类型安全 select DSL
- 启动时自动建表（示例；生产应使用迁移）

## 1. 准备 PostgreSQL

需要可连接的 PostgreSQL 18（或兼容版本）。示例默认连接：

```text
postgresql://127.0.0.1:5432/postgres
```

也可以用独立数据库：

```sql
CREATE DATABASE cboot_example;
```

并修改 `app.json` 的 `db.url`。

## 2. 配置密码

`app.json` 默认：

```json
"password": "${PGPASSWORD}"
```

三种方式任选：

1. 环境变量（推荐）：

```cmd
set PGPASSWORD=你的密码
```

PowerShell：

```powershell
$env:PGPASSWORD = "你的密码"
```

2. 配置里直接写：

```json
"password": "你的密码"
```

3. 删掉 `password` 字段：libpq 会读取 `PGPASSWORD` 环境变量。

`${PGPASSWORD}` 未展开时，组件会把它当作空密码并回退到 libpq 的
`PGPASSWORD`，不会把字面量 `${PGPASSWORD}` 当密码。

## 3. 安装 cboot SDK

在 cboot 仓库根目录：

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release
cmake --install build-release --prefix build-release/install
```

## 4. 构建示例

```bash
cmake -S examples/orm -B examples/orm/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build-release/install" \
  -DCMAKE_CXX_COMPILER="<g++16 路径>"
cmake --build examples/orm/build
```

如果 `pg_config` 不在 PATH：

```bash
cmake -S examples/orm -B examples/orm/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build-release/install" \
  -DPostgreSQL_ROOT="C:/SQL_DB/PostgreSQL/18" \
  -DPostgreSQL_LIBRARY_RELEASE="C:/SQL_DB/PostgreSQL/18/lib/libpq.lib" \
  -DPostgreSQL_LIBRARY_DEBUG="C:/SQL_DB/PostgreSQL/18/lib/libpq.lib"
```

Windows 下运行时需要 `libpq.dll` 在 PATH 或 exe 同目录；PostgreSQL 安装的
`bin` 目录通常已经在 PATH。

## 5. 运行

`app.json` 是相对工作目录的，从示例目录启动：

```bash
cd examples/orm
./build/cboot_example_orm --server.port=8080
```

启动时会：

1. 加载 `app.json`
2. 构造 `PgDataSource` 并打开连接池
3. 构造 `EntityManager`
4. `OrmSetup` 的 `PostConstruct` 执行 `create_schema<User>()`
5. 挂载 /api/orm 路由

数据库不可达时启动失败，属于预期行为。

## 6. 调用

```bash
# 创建
curl -X POST -H "Content-Type: application/json" \
  -d '{"name":"alice","email":"alice@example.com","active":true}' \
  http://127.0.0.1:8080/api/orm/users

# 查询单条（id 用上一步返回的值）
curl http://127.0.0.1:8080/api/orm/users/1

# 列表（DSL order_by）
curl http://127.0.0.1:8080/api/orm/users

# 删除
curl -X DELETE http://127.0.0.1:8080/api/orm/users/1

# 事务：两条都成功才提交
curl -X POST -H "Content-Type: application/json" \
  -d '{"users":[{"name":"bob"},{"name":"carol"}]}' \
  http://127.0.0.1:8080/api/orm/users/batch

# 事务失败：第二条 name 为空 → 整体回滚，返回 400
curl -X POST -H "Content-Type: application/json" \
  -d '{"users":[{"name":"dave"},{"name":""}]}' \
  http://127.0.0.1:8080/api/orm/users/batch
```

批量接口在 `transaction` 内插入；任意一条失败会抛异常，事务自动
`ROLLBACK`，外层 `catch` 返回 400。失败请求不会留下部分数据。

## 7. 配置键

| 键 | 说明 |
|---|---|
| `db.url` | PostgreSQL URI 或 keyword=value conninfo |
| `db.user` | 覆盖 URL 中的用户 |
| `db.password` | 密码；支持 `${PGPASSWORD}`，留空回退环境变量 |
| `db.sslmode` | `disable` / `prefer` / `require` 等 |
| `db.pool_size` | 连接池大小 |
| `db.connect_timeout_s` | 建连超时（秒） |
| `db.acquire_timeout_ms` | 从池中取连接超时（毫秒） |

## 8. 清理

示例表：

```sql
DROP TABLE IF EXISTS public.cboot_example_users;
```

生产环境不要依赖 `create_schema` 自动建表，使用迁移脚本管理 schema。
