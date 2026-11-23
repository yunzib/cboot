# ============================================================================
# cboot.auth.postgres — PostgreSQL TokenStore 驱动
#
# 贡献：PgTokenStore（持久化登录态）。
# 依赖：cboot.auth（TokenStore/AuthService/AuthGuard）与
#       cboot.orm.postgres（EntityManager/DataSource）。
# 包名 cboot.auth.postgres；target cboot::auth_postgres。
# ============================================================================
if(NOT TARGET cboot_orm_postgres)
    message(STATUS "cboot.auth.postgres: cboot.orm.postgres not available; skipping")
    return()
endif()

cboot_component(auth_postgres
    PACKAGE_NAME cboot.auth.postgres
    TIER contract
    VERSION ${CBOOT_VERSION}
    MODULES auth.postgres.cppm
    SOURCES src/postgres.cpp
    DEPENDS meta core config json web auth orm orm_postgres
    PACKAGE_DEPENDS cboot.auth cboot.orm.postgres
    TEST_SOURCES test/auth_postgres_smoke.cpp
)

if(TARGET cboot_auth_postgres_test)
    if(NOT PostgreSQL_FOUND)
        find_package(PostgreSQL QUIET)
    endif()
    if(TARGET PostgreSQL::PostgreSQL)
        target_link_libraries(cboot_auth_postgres_test PRIVATE PostgreSQL::PostgreSQL)
    endif()
    if(WIN32)
        target_link_libraries(cboot_auth_postgres_test PRIVATE ws2_32)
    endif()
endif()
