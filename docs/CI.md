# CI

## 1. 平台要求

cboot-framework 依赖 GCC 16.2 的 P-2996 反射实现，GitHub 托管 runner
不具备该工具链。CI 使用 **self-hosted Windows runner**：

- Windows x64
- GCC 16.2（`g++` 在 PATH）
- CMake 4.3+
- Ninja
- PostgreSQL 客户端（启用 PostgreSQL 集成测试时）
- MySQL 8 客户端（启用 MySQL 集成测试时）

## 2. 脚本

`scripts/ci.ps1` 执行：

1. `cmake --preset gcc` / `gcc-release`
2. `cmake --build`
3. `ctest --output-on-failure`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci.ps1 `
  -Config Release `
  -MysqlRoot C:\SQL_DB\mysql-8.2.0 `
  -PostgresRoot C:\SQL_DB\PostgreSQL\18
```

`-RequireDb` 会检查 `CBOOT_TEST_PG_URL` 和 `CBOOT_TEST_MYSQL_DATABASE`
是否设置；未设置时直接失败，避免 CI 静默跳过数据库集成。

## 3. GitHub workflow

`.github/workflows/ci.yml` 使用：

```yaml
runs-on: [self-hosted, windows, x64]
```

Repository variables：

| 变量 | 示例 |
|---|---|
| `CBOOT_MYSQL_ROOT` | `C:\SQL_DB\mysql-8.2.0` |
| `CBOOT_POSTGRES_ROOT` | `C:\SQL_DB\PostgreSQL\18` |
| `CBOOT_REQUIRE_DB` | `true` 时要求数据库集成测试不能跳过 |

Repository secrets（设置后 PostgreSQL/MySQL 集成测试会真实执行）：

| Secret | 说明 |
|---|---|
| `CBOOT_TEST_PG_URL` | PostgreSQL URL |
| `CBOOT_TEST_MYSQL_DATABASE` | 测试库 |
| `CBOOT_TEST_MYSQL_USER` | 用户 |
| `CBOOT_TEST_MYSQL_PASSWORD` | 密码 |
| `CBOOT_TEST_MYSQL_HOST` | 可选，默认 127.0.0.1 |
| `CBOOT_TEST_MYSQL_PORT` | 可选，默认 3306 |

## 4. 测试范围

设置数据库变量后，CI 会额外执行：

- ORM PostgreSQL/MySQL CRUD、事务、`query<T>`
- `cboot.jwt` HS256/RS256 单元测试
- `auth_postgres_smoke`：PgTokenStore 持久化/撤销/踢人

未设置时这些集成测试会跳过，单元测试仍会执行。

## 5. 本地等价命令

```bash
cmake --preset gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

Release：

```bash
cmake --preset gcc-release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```
