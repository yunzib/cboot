# ============================================================================
# cboot.orm.mysql — MySQL 驱动契约组件
#
# 贡献：MysqlProps（配置）、MysqlDataSource（libmysql 连接池）。
# 依赖：通用 orm_component；第三方客户端通过随 SDK 安装的 FindMySQL.cmake
# 查找，不进入模块接口。
# 包名 cboot.orm.mysql；target 名 cboot::orm_mysql。
# ============================================================================

# 使用仓库/安装 SDK 自带的 FindMySQL.cmake；MYSQL_ROOT 或 MySQL_ROOT 可覆盖。
list(APPEND CMAKE_MODULE_PATH "${CBOOT_CMAKE_DIR}")
find_package(MySQL QUIET)

if(NOT MySQL_FOUND)
    message(STATUS
        "cboot.orm.mysql: MySQL client not found; "
        "set MySQL_ROOT/MYSQL_ROOT or MySQL_INCLUDE_DIR/MySQL_LIBRARY to enable")
    return()
endif()

cboot_component(orm_mysql
    PACKAGE_NAME cboot.orm.mysql
    TIER contract
    VERSION 0.1.0
    MODULES orm.mysql.cppm
    SOURCES src/mysql.cpp
    DEPENDS meta core config log orm
    PACKAGE_DEPENDS cboot.orm MySQL
    TEST_SOURCES test/mysql_smoke.cpp
)

target_link_libraries(cboot_orm_mysql PRIVATE MySQL::Client)
if(WIN32)
    target_link_libraries(cboot_orm_mysql PRIVATE ws2_32)
endif()

if(TARGET cboot_orm_mysql_test)
    target_link_libraries(cboot_orm_mysql_test PRIVATE MySQL::Client cboot_app)
    if(WIN32)
        target_link_libraries(cboot_orm_mysql_test PRIVATE ws2_32)
        # Windows 运行时需要 libmysql.dll；复制到测试 exe 同目录，
        # 避免依赖调用方 PATH（安装/示例仍建议把 MySQL lib 加入 PATH）。
        get_filename_component(_mysql_lib_dir "${MySQL_LIBRARY}" DIRECTORY)
        if(EXISTS "${_mysql_lib_dir}/libmysql.dll")
            add_custom_command(TARGET cboot_orm_mysql_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mysql_lib_dir}/libmysql.dll"
                    "$<TARGET_FILE_DIR:cboot_orm_mysql_test>")
        endif()
    endif()
endif()
