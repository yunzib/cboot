# ============================================================================
# cboot.orm.postgres — PostgreSQL 驱动契约组件
#
# 贡献：PgProps（配置）、PgDataSource（libpq 连接池）。
# 依赖：通用 orm_component（EntityManager）与 libpq。
# 包名 cboot.orm.postgres；target 名 cboot::orm_postgres。
# ============================================================================

# 优先用 pg_config 推导 PostgreSQL_ROOT；也可由 preset/命令行覆盖。
if(NOT PostgreSQL_FOUND)
    find_program(PG_CONFIG pg_config)
    if(PG_CONFIG)
        get_filename_component(_pgbin "${PG_CONFIG}" DIRECTORY)
        get_filename_component(PostgreSQL_ROOT "${_pgbin}" DIRECTORY)
    endif()

    # MinGW 下 FindPostgreSQL 可能选中 libpq.a（静态/MSVC 归档），
    # 链接会缺 libintl/ws2_32/security cookie；优先用 DLL 的 import library。
    if(WIN32 AND PostgreSQL_ROOT
       AND EXISTS "${PostgreSQL_ROOT}/lib/libpq.lib")
        set(_pq_import "${PostgreSQL_ROOT}/lib/libpq.lib")
        # FindPostgreSQL 的导入目标按配置取 *_RELEASE/*_DEBUG；
        # 只改 PostgreSQL_LIBRARY 不够。
        set(PostgreSQL_LIBRARY "${_pq_import}"
            CACHE FILEPATH "PostgreSQL client import library" FORCE)
        set(PostgreSQL_LIBRARY_RELEASE "${_pq_import}"
            CACHE FILEPATH "PostgreSQL client import library (release)" FORCE)
        set(PostgreSQL_LIBRARY_DEBUG "${_pq_import}"
            CACHE FILEPATH "PostgreSQL client import library (debug)" FORCE)
    endif()

    find_package(PostgreSQL QUIET)
endif()

if(NOT PostgreSQL_FOUND)
    message(STATUS "cboot.orm.postgres: PostgreSQL/libpq not found; skipping component")
    return()
endif()

cboot_component(orm_postgres
    PACKAGE_NAME cboot.orm.postgres
    TIER contract
    VERSION 0.1.0
    MODULES orm.postgres.cppm
    SOURCES src/postgres.cpp
    DEPENDS meta core config orm
    PACKAGE_DEPENDS cboot.orm PostgreSQL
    TEST_SOURCES test/postgres_smoke.cpp
)

# libpq 只被实现单元使用；模块接口不暴露 libpq 头。
target_link_libraries(cboot_orm_postgres PRIVATE PostgreSQL::PostgreSQL)
if(WIN32)
    target_link_libraries(cboot_orm_postgres PRIVATE ws2_32)
endif()

# 组件测试直接链接 libpq：静态库的 PRIVATE 依赖在链接期仍需解析。
if(TARGET cboot_orm_postgres_test)
    target_link_libraries(cboot_orm_postgres_test
        PRIVATE PostgreSQL::PostgreSQL cboot_app)
    if(WIN32)
        target_link_libraries(cboot_orm_postgres_test PRIVATE ws2_32)
    endif()
endif()
