cmake_minimum_required(VERSION 4.3.2)

# 消费方口径：必须在 project() 之前设置（CMake 4.3 + GCC16 模块/import std）
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(cboot_example_orm LANGUAGES CXX)

# MinGW 下 PostgreSQL 官方包同时带 libpq.a / libpq.lib；
# FindPostgreSQL 可能选中静态 libpq.a，链接缺 intl/ws2_32/security。
# 示例在 find_package(cboot.orm.postgres) 之前优先选 DLL 的 import library。
if(WIN32)
    find_program(PG_CONFIG pg_config)
    if(PG_CONFIG)
        get_filename_component(_pgbin "${PG_CONFIG}" DIRECTORY)
        get_filename_component(PostgreSQL_ROOT "${_pgbin}" DIRECTORY)
    endif()
    if(PostgreSQL_ROOT AND EXISTS "${PostgreSQL_ROOT}/lib/libpq.lib")
        set(_pq_import "${PostgreSQL_ROOT}/lib/libpq.lib")
        set(PostgreSQL_LIBRARY "${_pq_import}" CACHE FILEPATH "" FORCE)
        set(PostgreSQL_LIBRARY_RELEASE "${_pq_import}" CACHE FILEPATH "" FORCE)
        set(PostgreSQL_LIBRARY_DEBUG "${_pq_import}" CACHE FILEPATH "" FORCE)
    endif()
endif()

find_package(cboot REQUIRED)
find_package(cboot.orm.postgres REQUIRED)

add_executable(cboot_example_orm main.cpp)
target_link_libraries(cboot_example_orm PRIVATE
    cboot::app
    cboot::orm_postgres)
