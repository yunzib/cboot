# ============================================================================
# ci.ps1 — cboot-framework 本地/self-hosted Windows CI
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci.ps1 `
#       -Config Release -MysqlRoot C:\SQL_DB\mysql-8.2.0 `
#       -PostgresRoot C:\SQL_DB\PostgreSQL\18 -RequireDb
#
# 数据库集成测试环境变量：
#   CBOOT_TEST_PG_URL
#   CBOOT_TEST_MYSQL_DATABASE / USER / PASSWORD（可选 HOST/PORT）
# ============================================================================

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [string]$MysqlRoot = "",
    [string]$PostgresRoot = "",
    [switch]$RequireDb
)

$ErrorActionPreference = "Stop"

$preset = if ($Config -eq "Release") { "gcc-release" } else { "gcc" }
$buildDir = if ($Config -eq "Release") { "build-release" } else { "build" }

$configureArgs = @("--preset", $preset)
if ($MysqlRoot) {
    $configureArgs += "-DMYSQL_ROOT=$MysqlRoot"
}
if ($PostgresRoot) {
    $configureArgs += "-DPostgreSQL_ROOT=$PostgresRoot"
    $pq = Join-Path $PostgresRoot "lib/libpq.lib"
    if (Test-Path $pq) {
        # MinGW 下强制选择 DLL import library，避免 FindPostgreSQL 选 libpq.a
        $configureArgs += "-DPostgreSQL_LIBRARY_RELEASE=$pq"
        $configureArgs += "-DPostgreSQL_LIBRARY_DEBUG=$pq"
    }
}

Write-Host "==> configure ($Config)"
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "==> build ($Config)"
& cmake --build $buildDir --parallel 8
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if ($RequireDb) {
    if (-not $env:CBOOT_TEST_PG_URL) {
        throw "CBOOT_TEST_PG_URL is required by -RequireDb"
    }
    if (-not $env:CBOOT_TEST_MYSQL_DATABASE) {
        throw "CBOOT_TEST_MYSQL_DATABASE is required by -RequireDb"
    }
}

Write-Host "==> ctest ($Config)"
& ctest --test-dir $buildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "ctest failed" }

Write-Host "==> CI PASS ($Config)"
