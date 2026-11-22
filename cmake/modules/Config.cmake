# ============================================================================
# cboot.config — 配置绑定（properties + 反射结构体）
# ============================================================================
add_library(cboot_config)
target_compile_features(cboot_config PUBLIC cxx_std_26)

target_sources(cboot_config
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/config
        FILES
            modules/config/config.cppm

    PRIVATE
        src/config/config.cpp
)

# 注意：导出（find_package）场景下，模块提供者须**直接**链接——
# 传递依赖不参与 import 解析（config.cppm 直接 import cboot.meta.reflect）
target_link_libraries(cboot_config PUBLIC cboot_json cboot_meta)
