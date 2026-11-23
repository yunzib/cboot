# ============================================================================
# cboot.jwt — JWT 校验契约组件
#
# 贡献：JwtProps（配置）、JwtVerifier（HS256/RS256 校验）。
# 依赖：meta/core/config/json；OpenSSL 只在本组件实现单元使用。
# 包名 cboot.jwt；target cboot::jwt。
# ============================================================================

# CMake 的 FindOpenSSL 在 MinGW 工具链里通常需要显式 root；
# 若编译器自带 opt 目录，优先用它。
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
    message(STATUS "cboot.jwt: OpenSSL not found; skipping component")
    return()
endif()

cboot_component(jwt
    TIER contract
    VERSION ${CBOOT_VERSION}
    MODULES jwt.cppm
    SOURCES src/jwt.cpp
    DEPENDS meta core config json
    PACKAGE_DEPENDS OpenSSL
    TEST_SOURCES test/jwt_smoke.cpp
)

target_link_libraries(cboot_jwt PRIVATE OpenSSL::Crypto)

if(TARGET cboot_jwt_test)
    target_link_libraries(cboot_jwt_test PRIVATE OpenSSL::Crypto)
    if(WIN32 AND OPENSSL_ROOT_DIR)
        file(GLOB _openssl_crypto_dlls
            "${OPENSSL_ROOT_DIR}/bin/libcrypto*.dll")
        if(_openssl_crypto_dlls)
            add_custom_command(TARGET cboot_jwt_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_openssl_crypto_dlls}
                    "$<TARGET_FILE_DIR:cboot_jwt_test>")
        endif()
    endif()
endif()
