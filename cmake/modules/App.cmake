# ============================================================================
# cboot.app — 应用层（引导编排：自动发现 / 装配 / 启动）
#
# 依赖方向：app → web.server → routing/http → core/json/aop → meta
# ============================================================================
add_library(cboot_app)
target_compile_features(cboot_app PUBLIC cxx_std_26)

target_sources(cboot_app
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/app
        FILES
            modules/app/app.cppm

    PRIVATE
        src/app/app.cpp
)

# PUBLIC：消费方（main TU）需要 import web 的注解/响应类型与 app 本体
# 注：每个被直接 import 的模块都须直接链接（导出场景不做传递解析）
target_link_libraries(cboot_app
    PUBLIC
        cboot_aop
        cboot_config
        cboot_core
        cboot_json
        cboot_meta
        cboot_web
)
