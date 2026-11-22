# ============================================================================
# cboot.core — 依赖注入容器层（依赖 cboot.meta 反射基建）
#
# 内容：annotations.cppm（注解词汇表）+ context.cppm（编译期规划 + 运行时容器）
# ============================================================================
add_library(cboot_core)
target_compile_features(cboot_core PUBLIC cxx_std_26)

target_sources(cboot_core
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/core
        FILES
            modules/core/annotations.cppm
            modules/core/context.cppm
)

# cboot.meta 工具与 -freflection 传递（PUBLIC 于 cboot_meta，随链继承）
target_link_libraries(cboot_core PUBLIC cboot_config cboot_meta)
