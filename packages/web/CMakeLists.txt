# ============================================================================
# cboot.web — 路由 + 嵌入式 HTTP 服务器
#
# 依赖：core（run 糖的 DI）/ json（body DTO）/ meta（反射基建）
#       validation（body 字段校验）
# 平台：Windows（winsock2）；server.cppm 全局模块片段包含系统头
# ============================================================================
cboot_package(web
    VERSION ${CBOOT_VERSION}
    TIER runtime
    MODULES http.cppm parser.cppm session.cppm middleware.cppm error.cppm cors.cppm routing.cppm server.cppm
    SOURCES src/routing.cpp src/server.cpp src/iocp.cpp src/epoll.cpp
    DEPENDS aop async core json log meta validation
)

if(WIN32)
    # winsock2（PUBLIC：测试/消费方的回环客户端也需链接）
    target_link_libraries(cboot_web PUBLIC ws2_32)
endif()
