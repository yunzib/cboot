# ============================================================================
# cboot.json — 轻量 JSON（值模型 + 解析 + 反射绑定）
# 接口（modules/）+ 实现（src/）分离，kwik-ui 同款纪律
# ============================================================================
add_library(cboot_json)
target_compile_features(cboot_json PUBLIC cxx_std_26)

target_sources(cboot_json
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/modules/json
        FILES
            modules/json/json.cppm

    PRIVATE
        src/json/json.cpp
)

target_link_libraries(cboot_json PUBLIC cboot_meta)
