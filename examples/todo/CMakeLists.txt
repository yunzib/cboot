cmake_minimum_required(VERSION 4.3.2)

# 消费方口径：必须在 project() 之前设置（CMake 4.3 + GCC16 模块/import std）
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(cboot_example_todo LANGUAGES CXX)

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
find_package(cboot.auth REQUIRED)
find_package(cboot.auth.postgres REQUIRED)
find_package(cboot.orm.postgres REQUIRED)

# 密码哈希用 OpenSSL::Crypto；MinGW 编译器自带的 opt 目录优先。
if(NOT OpenSSL_FOUND)
    get_filename_component(_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_compiler_prefix "${_compiler_bin}" DIRECTORY)
    if(EXISTS "${_compiler_prefix}/opt/include/openssl/evp.h")
        set(OPENSSL_ROOT_DIR "${_compiler_prefix}/opt"
            CACHE PATH "OpenSSL root" FORCE)
    endif()
    find_package(OpenSSL QUIET)
endif()

if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR "cboot_example_todo: OpenSSL not found (password hashing)")
endif()

add_executable(cboot_example_todo main.cpp)

# 分层模块：model → repository → service → api → app
target_sources(cboot_example_todo PRIVATE
    FILE_SET cxx_modules TYPE CXX_MODULES
    FILES
        src/model.cppm
        src/security.cppm
        src/repository.cppm
        src/service.cppm
        src/setup.cppm
        src/api/auth_api.cppm
        src/api/errors.cppm
        src/api/task_api.cppm
        src/api/page_api.cppm
        src/app.cppm
)
target_sources(cboot_example_todo PRIVATE src/security.cpp)

# 模块导入方必须直接链接 provider（导出场景不解析传递依赖）：
# cboot.core.annotations / web.http / meta.reflect / validation / auth 分别对应下列 target。
target_link_libraries(cboot_example_todo PRIVATE
    cboot::app
    cboot::auth
    cboot::auth_postgres
    cboot::core
    cboot::meta
    cboot::validation
    cboot::web
    cboot::orm_postgres
    OpenSSL::Crypto)

# Windows 运行时把 libcrypto DLL 复制到 exe 同目录。
if(WIN32 AND OPENSSL_ROOT_DIR)
    file(GLOB _openssl_crypto_dlls "${OPENSSL_ROOT_DIR}/bin/libcrypto*.dll")
    if(_openssl_crypto_dlls)
        add_custom_command(TARGET cboot_example_todo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_openssl_crypto_dlls}
                "$<TARGET_FILE_DIR:cboot_example_todo>")
    endif()
endif()
