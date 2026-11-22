# ============================================================================
# cboot.web — 路由 + 嵌入式 HTTP 服务器
# 依赖：core（run 糖的 DI）/ json（body DTO）/ meta（反射基建）
# 平台：Windows（winsock2）；server.cppm 全局模块片段包含系统头
# ============================================================================
add_library(cboot_web)
target_compile_features(cboot_web PUBLIC cxx_std_26)

target_sources(cboot_web
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/web
        FILES
            modules/web/http.cppm
            modules/web/parser.cppm
            modules/web/session.cppm
            modules/web/routing.cppm
            modules/web/server.cppm

    PRIVATE
        src/web/routing.cpp
        src/web/server.cpp
        src/web/iocp.cpp
        src/web/epoll.cpp
)

target_link_libraries(cboot_web
    PUBLIC
        cboot_aop
        cboot_async      # task<Response> handler（routing 直接 import）
        cboot_core
        cboot_json
        cboot_meta
)

if(WIN32)
    # winsock2（PUBLIC：测试/消费方的回环客户端也需链接）
    target_link_libraries(cboot_web PUBLIC ws2_32)
endif()
