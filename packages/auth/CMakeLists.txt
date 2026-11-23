# ============================================================================
# cboot.auth — 认证契约组件
#
# 贡献：AuthProps、MemoryTokenStore、AuthService、AuthGuard。
# 依赖：core/config/web；不依赖 cboot.jwt（JWT 是独立组件）。
# 包名 cboot.auth；target cboot::auth。
# ============================================================================
cboot_component(auth
    TIER contract
    VERSION ${CBOOT_VERSION}
    MODULES auth.cppm
    SOURCES src/auth.cpp
    DEPENDS meta core config web
    TEST_SOURCES test/auth_smoke.cpp
)

if(WIN32)
    # BCryptGenRandom：opaque token 的平台 CSPRNG
    target_link_libraries(cboot_auth PRIVATE bcrypt)
endif()

# 组件测试校验 app_beans_t 闭包；auth 测试使用内存 store，不依赖 OpenSSL/JWT。
if(TARGET cboot_auth_test)
    target_link_libraries(cboot_auth_test PRIVATE cboot_app)
    if(WIN32)
        target_link_libraries(cboot_auth_test PRIVATE bcrypt)
    endif()
endif()
