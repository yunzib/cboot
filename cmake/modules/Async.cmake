# ============================================================================
# cboot.async — 协程基建（task<T> + 事件桥；纯模板，无运行时依赖）
# ============================================================================
add_library(cboot_async)
target_compile_features(cboot_async PUBLIC cxx_std_26)

target_sources(cboot_async
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/async
        FILES
            modules/async/async.cppm
)
