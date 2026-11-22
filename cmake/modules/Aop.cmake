# ============================================================================
# cboot.aop — 编译期切面（纯模板，无运行时依赖）
# ============================================================================
add_library(cboot_aop)
target_compile_features(cboot_aop PUBLIC cxx_std_26)

target_sources(cboot_aop
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/aop
        FILES
            modules/aop/aop.cppm
)
